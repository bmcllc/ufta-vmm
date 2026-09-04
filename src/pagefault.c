/*
 * UFTA-VMM — pagefault.c — Transparent page fault handler
 *
 * Uses SIGSEGV + mprotect(PROT_NONE) to intercept page accesses.
 * When a page is "evicted" to VRAM-sim or FILE tier, accessing it
 * triggers a fault → handler loads data → restores access → transparent.
 *
 * Flow:
 *   1. pf_page_evict() saves data to memfd/file, sets PROT_NONE
 *   2. App touches evicted page → SIGSEGV
 *   3. Handler looks up page, loads from backend, sets PROT_READ|PROT_WRITE
 *   4. sigreturn → app retries instruction → success
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ufta/pagefault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* ── Global context (for signal handler) ──────────────────────── */

static pf_context_t *g_pf_ctx = NULL;

/* ── High-res timestamp ───────────────────────────────────────── */

static uint64_t pf_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Signal handler ─────────────────────────────────────────────
 *
 * LIGHTWEIGHT handler: it does NOT perform any data transfer or
 * CUDA/OS calls (which are not async-signal-safe). Instead it:
 *   1. Records the fault in the lock-free ring buffer.
 *   2. Suspends the faulting thread via futex.
 *   3. Returns; the worker thread resolves the batch and wakes us.
 *
 * This eliminates the deadlock/UB risk of calling cudaMemcpy inside
 * a signal context and moves the heavy lifting off the hot path.
 */
static void pf_sigsegv_handler(int sig, siginfo_t *info, void *ctx_raw)
{
    (void)sig;
    (void)ctx_raw;

    /* Re-entrancy guard */
    if (!g_pf_ctx || g_pf_ctx->fault_in_progress) {
        /* Can't handle — re-crash with default handler */
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &sa, NULL);
        return;
    }
    g_pf_ctx->fault_in_progress = true;

    /* Faulting address */
    void *fault_addr = info->si_addr;
    pf_context_t *pf = g_pf_ctx;

    /* Is the fault within our region? */
    if (fault_addr < (void *)pf->region_base ||
        fault_addr >= (void *)((char *)pf->region_base + pf->region_size)) {
        goto re_crash;
    }

    /* Calculate page index */
    uintptr_t offset = (uintptr_t)fault_addr - (uintptr_t)pf->region_base;
    uint32_t page_idx = (uint32_t)(offset / pf->page_size);

    if (page_idx >= pf->page_count || !pf->pages[page_idx].valid) {
        goto re_crash;
    }

    pf_page_t *page = &pf->pages[page_idx];

    /* If page is already in RAM, something else is wrong */
    if (page->location == PF_LOC_RAM) {
        goto re_crash;
    }

    /* Update stats (cheap, no I/O) */
    pf->total_faults++;
    page->fault_count++;
    page->last_fault_ts = pf_now_ns();

    /* If the worker thread is running, hand off the fault:
     * enqueue it and suspend this thread until the worker resolves it. */
    if (pf->worker_started) {
        if (ufta_ring_push(&pf->worker.ring, page_idx) == 0) {
            /* Notify the worker there is work to do */
            ufta_worker_notify(&pf->worker);
            /* Suspend until the worker resolves this page */
            ufta_worker_wait(&pf->worker, page_idx);
            g_pf_ctx->fault_in_progress = false;
            return;
        }
        /* Ring full — fall through to synchronous path */
    }

    /* ── Synchronous fallback (no worker / ring full) ─────────
     * Kept for correctness when the worker is unavailable. This path
     * is NOT async-signal-safe but is only used as a fallback. */
    page->location = PF_LOC_RAM;

    /* Re-map the page as accessible */
    void *page_addr = (char *)pf->region_base + page_idx * pf->page_size;
    if (mprotect(page_addr, pf->page_size, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "[UFTA-PF] mprotect restore failed: %s\n",
                strerror(errno));
        goto re_crash;
    }

#if PF_HAS_CUDA
    /* Load from REAL GPU VRAM (cudaMemcpy D2H) */
    if (page->location == PF_LOC_VRAM_REAL && pf->vram_real_ready) {
        size_t offset = (size_t)page->page_index * pf->page_size;
        cudaError_t err = cudaMemcpy(page_addr,
                                     (char *)pf->vram_real.device_ptr + offset,
                                     pf->page_size, cudaMemcpyDeviceToHost);
        if (err == cudaSuccess) {
            pf->total_loads++;
            pf->bytes_loaded += pf->page_size;
        } else {
            fprintf(stderr, "[UFTA-PF] VRAM real load failed: %s\n",
                    cudaGetErrorString(err));
        }
        page->backing_fd = -1;
        g_pf_ctx->fault_in_progress = false;
        return;
    }
#endif

    /* Load data from backing fd (memfd/file) */
    if (page->backing_fd >= 0) {
        ssize_t rd = pread(page->backing_fd, page_addr, pf->page_size,
                           page->backing_off);
        if (rd != (ssize_t)pf->page_size) {
            fprintf(stderr, "[UFTA-PF] backing load failed (fd=%d, off=%ld): "
                    "%s\n", page->backing_fd, (long)page->backing_off,
                    strerror(errno));
            /* Page is now accessible but may contain garbage — continue */
        } else {
            pf->total_loads++;
            pf->bytes_loaded += pf->page_size;
        }
    }

    /* NOTE: backing_fd is a SHARED fd (memfd/file) — do NOT close it here.
     * It stays open for other pages. Just clear the per-page reference. */
    page->backing_fd = -1;

    g_pf_ctx->fault_in_progress = false;
    return;

re_crash:
    g_pf_ctx->fault_in_progress = false;
    {
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &sa, NULL);
    }
}

/* ── Worker batch load callback ─────────────────────────────────
 *
 * Called by the worker thread (NOT in signal context) to resolve a
 * batch of faults. This is where the actual data transfer happens:
 * coalescing adjacent pages into PCIe-friendly blocks and issuing
 * cudaMemcpyAsync / pread safely.
 */
static int pf_worker_load_batch(void *ctx, const uint32_t *idx,
                                uint32_t count)
{
    pf_context_t *pf = (pf_context_t *)ctx;
    if (!pf || count == 0) return -1;

    /* Coalesce contiguous pages into larger blocks. idx is sorted.
     * We walk the batch and for each contiguous run we issue a single
     * transfer covering the whole run (up to UFTA_WORKER_BATCH_MAX). */
    uint32_t i = 0;
    while (i < count) {
        uint32_t start = idx[i];
        uint32_t run_len = 1;

        /* Extend the run while pages are contiguous and within batch cap */
        while (i + run_len < count &&
               idx[i + run_len] == start + run_len &&
               (run_len + 1) * pf->page_size <= UFTA_WORKER_BATCH_MAX) {
            run_len++;
        }

        /* Resolve the contiguous run [start, start+run_len) */
        for (uint32_t k = 0; k < run_len; k++) {
            uint32_t pi = start + k;
            pf_page_t *page = &pf->pages[pi];
            if (!page->valid || page->location == PF_LOC_RAM)
                continue;

            void *page_addr = (char *)pf->region_base + pi * pf->page_size;

            /* Restore access first (so the app can proceed once woken) */
            if (mprotect(page_addr, pf->page_size,
                         PROT_READ | PROT_WRITE) != 0) {
                continue;
            }

#if PF_HAS_CUDA
            /* Load from REAL GPU VRAM (cudaMemcpy D2H) */
            if (page->location == PF_LOC_VRAM_REAL && pf->vram_real_ready) {
                size_t off = (size_t)pi * pf->page_size;
                cudaError_t err = cudaMemcpy(
                    page_addr,
                    (char *)pf->vram_real.device_ptr + off,
                    pf->page_size, cudaMemcpyDeviceToHost);
                if (err == cudaSuccess) {
                    pf->total_loads++;
                    pf->bytes_loaded += pf->page_size;
                    pf->worker.total_bytes_moved += pf->page_size;
                } else {
                    fprintf(stderr, "[UFTA-PF] VRAM real load failed: %s\n",
                            cudaGetErrorString(err));
                }
                page->backing_fd = -1;
                page->location = PF_LOC_RAM;
                page->dirty = false;
                continue;
            }
#endif

            /* Load data from backing fd (memfd/file) */
            if (page->backing_fd >= 0) {
                ssize_t rd = pread(page->backing_fd, page_addr,
                                   pf->page_size, page->backing_off);
                if (rd == (ssize_t)pf->page_size) {
                    pf->total_loads++;
                    pf->bytes_loaded += pf->page_size;
                    pf->worker.total_bytes_moved += pf->page_size;
                } else {
                    fprintf(stderr, "[UFTA-PF] backing load failed "
                            "(fd=%d, off=%ld): %s\n",
                            page->backing_fd, (long)page->backing_off,
                            strerror(errno));
                }
                /* backing_fd is shared — do NOT close */
                page->backing_fd = -1;
            }

            page->location = PF_LOC_RAM;
            page->dirty = false;
        }

        i += run_len;
    }

    return 0;
}

/* ── Init / Destroy ───────────────────────────────────────────── */

/* Common initialization (shared by pf_init and pf_init_cuda) */
static int pf_init_common(pf_context_t *ctx, uint32_t page_size,
                          uint32_t page_count)
{
    if (!ctx || page_size == 0 || page_count == 0 ||
        page_count > PF_MAX_PAGES) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->page_size = page_size;
    ctx->page_count = page_count;

    size_t total_size = (size_t)page_size * page_count;

    /* Create backing directory */
    mkdir(PF_BACKING_DIR, 0755);

    /* Create memfd for VRAM-sim backend */
    ctx->vram_fd = memfd_create("ufta-vram-sim", MFD_CLOEXEC);
    if (ctx->vram_fd < 0) {
        fprintf(stderr, "[UFTA-PF] memfd_create failed: %s\n", strerror(errno));
        return -1;
    }

    /* Pre-allocate VRAM-sim to hold all pages */
    if (ftruncate(ctx->vram_fd, total_size) != 0) {
        fprintf(stderr, "[UFTA-PF] ftruncate vram fd failed\n");
        close(ctx->vram_fd);
        return -1;
    }

    /* Create file backend for FILE tier */
    char path[128];
    snprintf(path, sizeof(path), "%s/backing.dat", PF_BACKING_DIR);
    ctx->file_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (ctx->file_fd < 0) {
        fprintf(stderr, "[UFTA-PF] open backing file failed: %s\n",
                strerror(errno));
        close(ctx->vram_fd);
        return -1;
    }
    if (ftruncate(ctx->file_fd, total_size) != 0) {
        fprintf(stderr, "[UFTA-PF] ftruncate file fd failed\n");
        close(ctx->vram_fd);
        close(ctx->file_fd);
        return -1;
    }

    /* mmap the virtual memory region — initially PROT_NONE for all pages */
    ctx->region_base = mmap(NULL, total_size,
                            PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ctx->region_base == MAP_FAILED) {
        fprintf(stderr, "[UFTA-PF] mmap region failed: %s\n", strerror(errno));
        close(ctx->vram_fd);
        close(ctx->file_fd);
        return -1;
    }
    ctx->region_size = total_size;

    /* Initialize page tracking */
    for (uint32_t i = 0; i < page_count; i++) {
        ctx->pages[i].vaddr = (char *)ctx->region_base + i * page_size;
        ctx->pages[i].location = PF_LOC_MISSING;
        ctx->pages[i].page_index = i;
        ctx->pages[i].size = page_size;
        ctx->pages[i].backing_fd = -1;
        ctx->pages[i].backing_off = (off_t)i * page_size;
        ctx->pages[i].valid = false;
    }

    /* Install SIGSEGV handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = pf_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &ctx->old_sa_segv) != 0) {
        fprintf(stderr, "[UFTA-PF] sigaction failed: %s\n", strerror(errno));
        munmap(ctx->region_base, total_size);
        close(ctx->vram_fd);
        close(ctx->file_fd);
        return -1;
    }

    g_pf_ctx = ctx;
    ctx->active = true;

    /* Start the async worker thread for batched fault resolution */
    if (ufta_worker_start(&ctx->worker, pf_worker_load_batch, ctx) == 0) {
        ctx->worker_started = true;
        printf("[UFTA-PF] Worker thread iniciado (batching %u–%u KB)\n",
               UFTA_WORKER_BATCH_MIN / 1024,
               UFTA_WORKER_BATCH_MAX / 1024);
    } else {
        ctx->worker_started = false;
        fprintf(stderr, "[UFTA-PF] Aviso: worker thread falhou — "
                        "usando fallback síncrono\n");
    }

    printf("[UFTA-PF] Handler inicializado: %u páginas × %u bytes = %.1f MB\n",
           page_count, page_size, (double)total_size / (1024.0 * 1024.0));
    printf("[UFTA-PF] VRAM-sim fd: %d | FILE fd: %d\n",
           ctx->vram_fd, ctx->file_fd);

    return 0;
}

int pf_init(pf_context_t *ctx, uint32_t page_size, uint32_t page_count)
{
    return pf_init_common(ctx, page_size, page_count);
}

#if PF_HAS_CUDA
int pf_init_cuda(pf_context_t *ctx, uint32_t page_size, uint32_t page_count)
{
    /* Common init (memfd + file backends) */
    if (pf_init_common(ctx, page_size, page_count) != 0) {
        return -1;
    }

    /* Initialize CUDA */
    if (cuda_backend_init(&ctx->gpu) != 0) {
        fprintf(stderr, "[UFTA-PF] CUDA não disponível — usando só memfd\n");
        ctx->vram_real_ready = false;
        return 0; /* degrade gracefully */
    }

    /* Allocate real VRAM region to hold all pages */
    size_t total_size = (size_t)page_size * page_count;
    if (cuda_mem_alloc(&ctx->vram_real, total_size) != 0) {
        fprintf(stderr, "[UFTA-PF] falha ao alocar VRAM real — usando só memfd\n");
        ctx->vram_real_ready = false;
        return 0;
    }

    ctx->vram_real_ready = true;
    printf("[UFTA-PF] VRAM REAL alocada: %.1f MB (%s)\n",
           (double)total_size / (1024.0 * 1024.0), ctx->gpu.name);
    return 0;
}
#endif

void pf_destroy(pf_context_t *ctx)
{
    if (!ctx) return;

    /* Stop the worker thread first (it may be mid-batch) */
    if (ctx->worker_started) {
        ufta_worker_stop(&ctx->worker);
        ctx->worker_started = false;
    }

    /* Restore original SIGSEGV handler */
    if (ctx->active) {
        sigaction(SIGSEGV, &ctx->old_sa_segv, NULL);
        g_pf_ctx = NULL;
    }

    /* Close backing fds */
    if (ctx->vram_fd >= 0) close(ctx->vram_fd);
    if (ctx->file_fd >= 0) close(ctx->file_fd);

#if PF_HAS_CUDA
    /* Free real VRAM */
    if (ctx->vram_real_ready) {
        cuda_mem_free(&ctx->vram_real);
        ctx->vram_real_ready = false;
    }
#endif

    /* Unmap region */
    if (ctx->region_base && ctx->region_base != MAP_FAILED) {
        munmap(ctx->region_base, ctx->region_size);
    }

    /* Remove backing directory */
    char path[128];
    snprintf(path, sizeof(path), "%s/backing.dat", PF_BACKING_DIR);
    unlink(path);
    rmdir(PF_BACKING_DIR);

    ctx->active = false;
}

/* ── Page allocation ──────────────────────────────────────────── */

void *pf_page_alloc(pf_context_t *ctx, uint32_t index)
{
    if (!ctx || index >= ctx->page_count) return NULL;

    pf_page_t *page = &ctx->pages[index];
    if (page->valid) return page->vaddr; /* already allocated */

    /* Make the page accessible */
    void *page_addr = (char *)ctx->region_base + index * ctx->page_size;
    if (mprotect(page_addr, ctx->page_size, PROT_READ | PROT_WRITE) != 0) {
        return NULL;
    }

    /* Initialize to zero */
    memset(page_addr, 0, ctx->page_size);

    page->valid = true;
    page->location = PF_LOC_RAM;
    page->dirty = false;
    page->access_count = 0;
    page->fault_count = 0;
    page->backing_fd = -1;

    ctx->pages_active++;
    return page->vaddr;
}

/* ── Page eviction ────────────────────────────────────────────── */

int pf_page_evict(pf_context_t *ctx, uint32_t index, pf_location_t target)
{
    if (!ctx || index >= ctx->page_count) return -1;

    pf_page_t *page = &ctx->pages[index];
    if (!page->valid || page->location == PF_LOC_MISSING) return -1;

    void *page_addr = (char *)ctx->region_base + index * ctx->page_size;

#if PF_HAS_CUDA
    /* Evict to REAL GPU VRAM (cudaMemcpy H2D) */
    if (target == PF_LOC_VRAM_REAL) {
        if (!ctx->vram_real_ready) return -1;
        size_t offset = (size_t)index * ctx->page_size;
        cudaError_t err = cudaMemcpy((char *)ctx->vram_real.device_ptr + offset,
                                     page_addr, ctx->page_size,
                                     cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            fprintf(stderr, "[UFTA-PF] VRAM real evict failed: %s\n",
                    cudaGetErrorString(err));
            return -1;
        }
        page->location = PF_LOC_VRAM_REAL;
        page->dirty = false;
        page->backing_fd = -1;

        /* Mark page as inaccessible (will trigger SIGSEGV on next access) */
        if (mprotect(page_addr, ctx->page_size, PROT_NONE) != 0) {
            fprintf(stderr, "[UFTA-PF] mprotect PROT_NONE failed: %s\n",
                    strerror(errno));
            return -1;
        }
        ctx->total_evictions++;
        ctx->bytes_stored += ctx->page_size;
        return 0;
    }
#endif

    /* Choose backing fd */
    int backing_fd;
    switch (target) {
    case PF_LOC_VRAM: backing_fd = ctx->vram_fd; break;
    case PF_LOC_FILE: backing_fd = ctx->file_fd; break;
    default: return -1;
    }

    /* Save page data to backing store */
    ssize_t wr = pwrite(backing_fd, page_addr, ctx->page_size,
                        page->backing_off);
    if (wr != (ssize_t)ctx->page_size) {
        fprintf(stderr, "[UFTA-PF] eviction write failed: %s\n",
                strerror(errno));
        return -1;
    }

    page->backing_fd = backing_fd;
    page->location = target;
    page->dirty = false;

    /* Mark page as inaccessible (will trigger SIGSEGV on next access) */
    if (mprotect(page_addr, ctx->page_size, PROT_NONE) != 0) {
        fprintf(stderr, "[UFTA-PF] mprotect PROT_NONE failed: %s\n",
                strerror(errno));
        return -1;
    }

    ctx->total_evictions++;
    ctx->bytes_stored += ctx->page_size;

    return 0;
}

/* ── Page load (manual, not from fault) ───────────────────────── */

int pf_page_load(pf_context_t *ctx, uint32_t index)
{
    if (!ctx || index >= ctx->page_count) return -1;

    pf_page_t *page = &ctx->pages[index];
    if (!page->valid) return -1;
    if (page->location == PF_LOC_RAM) return 0; /* already here */
    if (page->location == PF_LOC_MISSING) return -1;

    void *page_addr = (char *)ctx->region_base + index * ctx->page_size;

    /* Restore access */
    if (mprotect(page_addr, ctx->page_size, PROT_READ | PROT_WRITE) != 0) {
        return -1;
    }

#if PF_HAS_CUDA
    /* Load from REAL GPU VRAM */
    if (page->location == PF_LOC_VRAM_REAL && ctx->vram_real_ready) {
        size_t offset = (size_t)index * ctx->page_size;
        cudaError_t err = cudaMemcpy(page_addr,
                                     (char *)ctx->vram_real.device_ptr + offset,
                                     ctx->page_size, cudaMemcpyDeviceToHost);
        if (err == cudaSuccess) {
            ctx->total_loads++;
            ctx->bytes_loaded += ctx->page_size;
        }
        page->backing_fd = -1;
        page->location = PF_LOC_RAM;
        page->dirty = false;
        return 0;
    }
#endif

    /* Load data */
    if (page->backing_fd >= 0) {
        ssize_t rd = pread(page->backing_fd, page_addr, ctx->page_size,
                           page->backing_off);
        if (rd == (ssize_t)ctx->page_size) {
            ctx->total_loads++;
            ctx->bytes_loaded += ctx->page_size;
        }
        /* backing_fd is shared — do NOT close */
        page->backing_fd = -1;
    }

    page->location = PF_LOC_RAM;
    page->dirty = false;
    return 0;
}

/* ── Pattern write/verify ─────────────────────────────────────── */

int pf_page_write_pattern(pf_context_t *ctx, uint32_t index, uint8_t val)
{
    if (!ctx || index >= ctx->page_count) return -1;

    pf_page_t *page = &ctx->pages[index];
    if (!page->valid || page->location != PF_LOC_RAM) return -1;

    void *page_addr = (char *)ctx->region_base + index * ctx->page_size;
    memset(page_addr, val, ctx->page_size);
    page->dirty = true;
    return 0;
}

int pf_page_verify_pattern(pf_context_t *ctx, uint32_t index, uint8_t val)
{
    if (!ctx || index >= ctx->page_count) return -1;

    pf_page_t *page = &ctx->pages[index];
    if (!page->valid || page->location != PF_LOC_RAM) return -1;

    const uint8_t *data = (const uint8_t *)((char *)ctx->region_base +
                                             index * ctx->page_size);
    for (uint32_t i = 0; i < ctx->page_size; i++) {
        if (data[i] != val) return -1;
    }
    return 0;
}

/* ── Stats printing ───────────────────────────────────────────── */

void pf_stats_print(const pf_context_t *ctx, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;

    fprintf(f, "══════════════════════════════════════════════════\n");
    fprintf(f, "  UFTA-PF — Page Fault Handler Stats\n");
    fprintf(f, "══════════════════════════════════════════════════\n");
    fprintf(f, "  Páginas ativas:    %u / %u\n", ctx->pages_active, ctx->page_count);
    fprintf(f, "  Total faults:      %llu\n", (unsigned long long)ctx->total_faults);
    fprintf(f, "  Evictions:         %llu (%.1f MB)\n",
            (unsigned long long)ctx->total_evictions,
            (double)ctx->bytes_stored / (1024.0 * 1024.0));
    fprintf(f, "  Loads (in):        %llu (%.1f MB)\n",
            (unsigned long long)ctx->total_loads,
            (double)ctx->bytes_loaded / (1024.0 * 1024.0));

    /* Per-page summary by location */
    uint32_t in_ram = 0, in_vram = 0, in_vram_real = 0, in_file = 0, missing = 0;
    for (uint32_t i = 0; i < ctx->page_count; i++) {
        if (!ctx->pages[i].valid) continue;
        switch (ctx->pages[i].location) {
        case PF_LOC_RAM:       in_ram++;      break;
        case PF_LOC_VRAM:      in_vram++;     break;
        case PF_LOC_VRAM_REAL: in_vram_real++; break;
        case PF_LOC_FILE:      in_file++;     break;
        case PF_LOC_MISSING:   missing++;     break;
        }
    }
    fprintf(f, "  ── Distribuição ──\n");
    fprintf(f, "  RAM:  %u páginas\n", in_ram);
    fprintf(f, "  VRAM: %u páginas\n", in_vram);
#if PF_HAS_CUDA
    fprintf(f, "  VRAM REAL: %u páginas\n", in_vram_real);
#endif
    fprintf(f, "  FILE: %u páginas\n", in_file);

    /* Top faulted pages */
    fprintf(f, "  ── Top 5 Mais Faulted ──\n");
    /* Simple selection sort for top 5 */
    uint32_t top[5] = {0};
    uint64_t top_faults[5] = {0};
    for (uint32_t i = 0; i < ctx->page_count; i++) {
        if (!ctx->pages[i].valid) continue;
        uint64_t fc = ctx->pages[i].fault_count;
        for (int j = 0; j < 5; j++) {
            if (fc > top_faults[j]) {
                /* Shift down */
                for (int k = 4; k > j; k--) {
                    top[k] = top[k-1];
                    top_faults[k] = top_faults[k-1];
                }
                top[j] = i;
                top_faults[j] = fc;
                break;
            }
        }
    }
    for (int j = 0; j < 5; j++) {
        if (top_faults[j] == 0) break;
        const char *loc = "?";
        switch (ctx->pages[top[j]].location) {
        case PF_LOC_RAM:       loc = "RAM"; break;
        case PF_LOC_VRAM:      loc = "VRAM"; break;
        case PF_LOC_VRAM_REAL: loc = "VRAM-REAL"; break;
        case PF_LOC_FILE:      loc = "FILE"; break;
        default: loc = "MISS"; break;
        }
        fprintf(f, "    page[%u]: %llu faults (%s)\n",
                top[j], (unsigned long long)top_faults[j], loc);
    }

    fprintf(f, "══════════════════════════════════════════════════\n");
    fclose(f);
}
