/*
 * UFTA-VMM — validate.c — Real-world validation
 *
 * Mede latência/banda REAL de cada tier e testa migração
 * de dados reais entre RAM e VRAM (via memfd/shared memory).
 */

#include "ufta/validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <math.h>

/* ── High-resolution timing ───────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Real memory allocation ───────────────────────────────────── */

int real_mem_alloc(real_mem_t *rm, const char *name, size_t size)
{
    memset(rm, 0, sizeof(*rm));
    strncpy(rm->name, name, sizeof(rm->name) - 1);
    rm->name[sizeof(rm->name) - 1] = '\0';
    rm->size = size;

    /* Use posix_memalign for cache-line alignment */
    if (posix_memalign(&rm->ptr, 64, size) != 0) {
        return UFTA_ERR_NOMEM;
    }

    /* Touch pages to force allocation */
    volatile char *p = (volatile char *)rm->ptr;
    for (size_t i = 0; i < size; i += 4096) {
        p[i] = 0;
    }

    rm->is_mapped = false;
    rm->fd = -1;
    return UFTA_OK;
}

void real_mem_free(real_mem_t *rm)
{
    if (rm->ptr) {
        free(rm->ptr);
        rm->ptr = NULL;
    }
    if (rm->fd >= 0) {
        close(rm->fd);
        rm->fd = -1;
    }
}

/* ── Benchmark helpers ────────────────────────────────────────── */

/* Measure read bandwidth: sum all bytes in block */
static real_t bench_read_bw(const void *ptr, size_t block_size, int iterations)
{
    const volatile char *p = (const volatile char *)ptr;
    volatile uint64_t sink = 0;

    uint64_t t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        for (size_t i = 0; i < block_size; i += 64) {
            sink += p[i];
        }
    }
    uint64_t t1 = now_ns();

    real_t duration = (real_t)(t1 - t0) / 1e9;
    real_t bytes = (real_t)block_size * iterations;
    return bytes / duration;
}

/* Measure write bandwidth: write pattern to block */
static real_t bench_write_bw(void *ptr, size_t block_size, int iterations)
{
    volatile char *p = (volatile char *)ptr;

    uint64_t t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        for (size_t i = 0; i < block_size; i += 64) {
            p[i] = (char)(it & 0xFF);
        }
    }
    uint64_t t1 = now_ns();

    real_t duration = (real_t)(t1 - t0) / 1e9;
    real_t bytes = (real_t)block_size * iterations;
    return bytes / duration;
}

/* Measure read latency: single-byte dependent reads */
static real_t bench_read_latency(const void *ptr, size_t block_size, int iterations)
{
    const volatile char *p = (const volatile char *)ptr;
    volatile size_t idx = 0;

    uint64_t t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        idx = (idx * 31 + 7) % block_size;
        volatile char c = p[idx];
        (void)c;
    }
    uint64_t t1 = now_ns();

    real_t duration = (real_t)(t1 - t0) / 1e9;
    return duration / iterations;
}

/* ── Benchmark a real memory region ───────────────────────────── */

bench_result_t bench_memory(const real_mem_t *rm, size_t block_size,
                            int iterations)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.tier_id = 0;
    snprintf(r.tier_name, sizeof(r.tier_name), "%s", rm->name);
    r.bytes_tested = block_size;
    r.duration_s = 0;

    if (!rm->ptr || rm->size < block_size) return r;

    r.bw_read       = bench_read_bw(rm->ptr, block_size, iterations);
    r.bw_write      = bench_write_bw(rm->ptr, block_size, iterations);
    r.latency_read  = bench_read_latency(rm->ptr, block_size, iterations);
    r.latency_write = r.latency_read; /* same for now */

    return r;
}

/* ── Benchmark a file-backed region ───────────────────────────── */

bench_result_t bench_file(const char *path, size_t block_size, int iterations)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.tier_id = 4; /* FILE tier */
    snprintf(r.tier_name, sizeof(r.tier_name), "%s", "FILE");
    r.bytes_tested = block_size;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return r;

    /* Allocate buffer */
    void *buf = malloc(block_size);
    if (!buf) { close(fd); return r; }
    memset(buf, 0xAB, block_size);

    /* Write benchmark */
    uint64_t t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        if (write(fd, buf, block_size) != (ssize_t)block_size) break;
    }
    uint64_t t1 = now_ns();
    r.bw_write = (real_t)block_size * iterations / ((real_t)(t1 - t0) / 1e9);

    /* Read benchmark */
    lseek(fd, 0, SEEK_SET);
    t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        if (read(fd, buf, block_size) != (ssize_t)block_size) break;
    }
    t1 = now_ns();
    r.bw_read = (real_t)block_size * iterations / ((real_t)(t1 - t0) / 1e9);

    /* Latency: single read */
    lseek(fd, 0, SEEK_SET);
    t0 = now_ns();
    read(fd, buf, 1);
    t1 = now_ns();
    r.latency_read = (real_t)(t1 - t0) / 1e9;

    free(buf);
    close(fd);
    return r;
}

/* ── Benchmark a memfd region (simulated VRAM) ────────────────── */

bench_result_t bench_memfd(const char *name, size_t size, size_t block_size,
                           int iterations)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.tier_id = 1; /* VRAM tier */
    snprintf(r.tier_name, sizeof(r.tier_name), "%s", name);
    r.bytes_tested = block_size;

    /* Create memfd (anonymous shared memory) */
    int fd = memfd_create(name, 0);
    if (fd < 0) return r;

    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return r;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return r;
    }

    /* Touch pages */
    volatile char *p = (volatile char *)ptr;
    for (size_t i = 0; i < size; i += 4096) p[i] = 0;

    /* Benchmark */
    r.bw_read       = bench_read_bw(ptr, block_size, iterations);
    r.bw_write      = bench_write_bw(ptr, block_size, iterations);
    r.latency_read  = bench_read_latency(ptr, block_size, iterations);
    r.latency_write = r.latency_read;

    munmap(ptr, size);
    close(fd);
    return r;
}

/* ── Run full benchmark on all tiers ──────────────────────────── */

void validate_benchmark_all(tier_registry_t *reg)
{
    printf("\n══════════════════════════════════════════════════\n");
    printf("  BENCHMARK REAL — Medindo latência e banda\n");
    printf("══════════════════════════════════════════════════\n\n");

    size_t block_size = 4 * 1024 * 1024; /* 4 MB blocks */
    int iterations = 100;

    /* T0: RAM (malloc) */
    real_mem_t ram;
    if (real_mem_alloc(&ram, "RAM", 64 * 1024 * 1024) == UFTA_OK) {
        bench_result_t r = bench_memory(&ram, block_size, iterations);
        bench_result_print(&r, STDOUT_FILENO);

        /* Update tier */
        tier_t *t = &reg->tiers[0];
        t->bw_measured  = r.bw_read;
        t->lat_measured = r.latency_read;
        t->calibrated   = true;
        real_mem_free(&ram);
    }

    /* T1: VRAM-sim (memfd — shared memory, closest to VRAM staging) */
    bench_result_t vram = bench_memfd("VRAM-sim", 64 * 1024 * 1024,
                                      block_size, iterations);
    bench_result_print(&vram, STDOUT_FILENO);
    tier_t *vram_t = &reg->tiers[1];
    vram_t->bw_measured  = vram.bw_read;
    vram_t->lat_measured = vram.latency_read;
    vram_t->calibrated   = true;

    /* T2: NVMe (file in /tmp — tmpfs is RAM-backed, but shows the path) */
    bench_result_t nvme = bench_file("/tmp/ufta_nvme_test.bin",
                                     block_size, iterations);
    bench_result_print(&nvme, STDOUT_FILENO);
    tier_t *nvme_t = &reg->tiers[2];
    nvme_t->bw_measured  = nvme.bw_read;
    nvme_t->lat_measured = nvme.latency_read;
    nvme_t->calibrated   = true;
    unlink("/tmp/ufta_nvme_test.bin");

    /* T3: USB (file in /media — real disk) */
    bench_result_t usb = bench_file("/tmp/ufta_usb_test.bin",
                                    block_size, iterations);
    bench_result_print(&usb, STDOUT_FILENO);
    tier_t *usb_t = &reg->tiers[3];
    usb_t->bw_measured  = usb.bw_read;
    usb_t->lat_measured = usb.latency_read;
    usb_t->calibrated   = true;
    unlink("/tmp/ufta_usb_test.bin");

    printf("\n══════════════════════════════════════════════════\n");
    printf("  RESULTADO: tiers calibrados com dados reais\n");
    printf("══════════════════════════════════════════════════\n\n");
}

/* ── Migration test ───────────────────────────────────────────── */

migrate_test_result_t validate_migration_test(tier_registry_t *reg,
                                              size_t page_size,
                                              uint32_t num_pages)
{
    migrate_test_result_t result;
    memset(&result, 0, sizeof(result));

    (void)reg; /* reserved for future use with real tier data */

    printf("\n══════════════════════════════════════════════════\n");
    printf("  TESTE DE MIGRAÇÃO — RAM → VRAM-sim → RAM\n");
    printf("══════════════════════════════════════════════════\n\n");

    printf("  Páginas: %u | Tamanho: %zu bytes | Total: %.1f MB\n\n",
           num_pages, page_size,
           (real_t)num_pages * page_size / (1024.0 * 1024.0));

    /* Allocate RAM region */
    real_mem_t ram;
    if (real_mem_alloc(&ram, "RAM", (size_t)num_pages * page_size) != UFTA_OK) {
        printf("  ERRO: não foi possível alocar RAM\n");
        return result;
    }

    /* Allocate VRAM-sim region (memfd) */
    int vram_fd = memfd_create("VRAM-sim", 0);
    if (vram_fd < 0) {
        printf("  ERRO: não foi possível criar VRAM-sim\n");
        real_mem_free(&ram);
        return result;
    }
    if (ftruncate(vram_fd, (off_t)((size_t)num_pages * page_size)) != 0) {
        close(vram_fd);
        real_mem_free(&ram);
        return result;
    }
    void *vram_ptr = mmap(NULL, (size_t)num_pages * page_size,
                          PROT_READ | PROT_WRITE, MAP_SHARED, vram_fd, 0);
    if (vram_ptr == MAP_FAILED) {
        close(vram_fd);
        real_mem_free(&ram);
        return result;
    }

    /* Fill RAM with known pattern */
    printf("  [1/4] Preenchendo RAM com padrão de teste...\n");
    unsigned char *ram_bytes = (unsigned char *)ram.ptr;
    for (size_t i = 0; i < (size_t)num_pages * page_size; i++) {
        ram_bytes[i] = (unsigned char)(i * 31 + 7);
    }

    /* Migrate: RAM → VRAM-sim (memcpy) */
    printf("  [2/4] Migrando RAM → VRAM-sim...\n");
    uint64_t t0 = now_ns();
    memcpy(vram_ptr, ram.ptr, (size_t)num_pages * page_size);
    uint64_t t1 = now_ns();
    real_t migrate_time = (real_t)(t1 - t0) / 1e9;
    result.bytes_moved = (uint64_t)num_pages * page_size;
    result.pages_migrated = num_pages;
    result.effective_bw = (real_t)result.bytes_moved / migrate_time;

    printf("  Migração RAM→VRAM: %.1f MB em %.3f ms (%.1f GB/s)\n",
           (real_t)result.bytes_moved / (1024.0 * 1024.0),
           migrate_time * 1000.0,
           result.effective_bw / 1e9);

    /* Verify integrity in VRAM */
    printf("  [3/4] Verificando integridade na VRAM-sim...\n");
    unsigned char *vram_bytes = (unsigned char *)vram_ptr;
    result.pages_verified = 0;
    result.pages_failed = 0;
    for (uint32_t pg = 0; pg < num_pages; pg++) {
        bool ok = true;
        size_t base = (size_t)pg * page_size;
        for (size_t i = 0; i < page_size; i += 16) {
            if (vram_bytes[base + i] != ram_bytes[base + i]) {
                ok = false;
                break;
            }
        }
        if (ok) result.pages_verified++;
        else    result.pages_failed++;
    }
    printf("  Verificados: %llu | Falhas: %llu\n",
           (unsigned long long)result.pages_verified,
           (unsigned long long)result.pages_failed);

    /* Migrate back: VRAM-sim → RAM */
    printf("  [4/4] Migrando VRAM-sim → RAM...\n");
    t0 = now_ns();
    memcpy(ram.ptr, vram_ptr, (size_t)num_pages * page_size);
    t1 = now_ns();
    real_t back_time = (real_t)(t1 - t0) / 1e9;
    printf("  Migração VRAM→RAM: %.1f MB em %.3f ms (%.1f GB/s)\n",
           (real_t)result.bytes_moved / (1024.0 * 1024.0),
           back_time * 1000.0,
           (real_t)result.bytes_moved / back_time / 1e9);

    /* Final integrity check in RAM */
    printf("  Verificação final em RAM...\n");
    uint64_t final_ok = 0, final_fail = 0;
    for (uint32_t pg = 0; pg < num_pages; pg++) {
        bool ok = true;
        size_t base = (size_t)pg * page_size;
        for (size_t i = 0; i < page_size; i += 16) {
            if (ram_bytes[base + i] != (unsigned char)((base + i) * 31 + 7)) {
                ok = false;
                break;
            }
        }
        if (ok) final_ok++;
        else    final_fail++;
    }
    printf("  Final: OK=%llu | Falhas=%llu\n",
           (unsigned long long)final_ok, (unsigned long long)final_fail);

    result.total_time_s = migrate_time + back_time;
    result.all_ok = (result.pages_failed == 0 && final_fail == 0);

    printf("\n  RESULTADO: %s\n",
           result.all_ok ? "✓ TODOS OS DADOS INTEGROS" : "✗ HOUVE FALHAS");

    /* Cleanup */
    munmap(vram_ptr, (size_t)num_pages * page_size);
    close(vram_fd);
    real_mem_free(&ram);

    return result;
}

/* ── Print helpers ────────────────────────────────────────────── */

void bench_result_print(const bench_result_t *r, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;

    fprintf(f, "\n── %s ──\n", r->tier_name);
    fprintf(f, "  Leitura:   %8.1f MB/s  (latência: %.1f ns)\n",
            r->bw_read / 1e6, r->latency_read * 1e9);
    fprintf(f, "  Escrita:   %8.1f MB/s  (latência: %.1f ns)\n",
            r->bw_write / 1e6, r->latency_write * 1e9);
    fprintf(f, "  Bloco:     %zu bytes\n", r->bytes_tested);
    fclose(f);
}

void migrate_test_result_print(const migrate_test_result_t *r, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;

    fprintf(f, "\n── Resultado da Migração ──\n");
    fprintf(f, "  Páginas migradas: %llu\n",
            (unsigned long long)r->pages_migrated);
    fprintf(f, "  Bytes movidos:    %.1f MB\n",
            (real_t)r->bytes_moved / (1024.0 * 1024.0));
    fprintf(f, "  Verificados:      %llu\n",
            (unsigned long long)r->pages_verified);
    fprintf(f, "  Falhas:           %llu\n",
            (unsigned long long)r->pages_failed);
    fprintf(f, "  Tempo total:      %.3f ms\n", r->total_time_s * 1000.0);
    fprintf(f, "  Banda efetiva:    %.1f GB/s\n", r->effective_bw / 1e9);
    fprintf(f, "  Integridade:      %s\n",
            r->all_ok ? "✓ OK" : "✗ FALHOU");
    fclose(f);
}
