/*
 * UFTA-VMM — worker.c — Asynchronous page-fault worker thread
 *
 * Implements the lock-free ring buffer, the worker thread loop with
 * batching, and futex-based suspension of faulting threads.
 *
 * The signal handler (producer) only pushes a fault request and
 * suspends the faulting thread. The worker (consumer) coalesces
 * adjacent faults into PCIe-friendly batches and performs the actual
 * data transfer off the signal hot path.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ufta/worker.h"
#include "ufta/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#endif

/* ── Futex helpers (lightweight thread suspension) ────────────── */

#ifdef _WIN32
/*
 * Windows: use CONDITION_VARIABLE + shared CRITICAL_SECTION to emulate
 * futex_wait / futex_wake.  The CRITICAL_SECTION is process-global
 * (like the Linux futex hash table) — adequate for the low waiter
 * count of the UFTA worker (≤256).
 */
static CRITICAL_SECTION g_futex_cs;
static CONDITION_VARIABLE g_futex_cv;
static BOOL g_futex_inited = FALSE;

static void futex_ensure_init(void)
{
    if (!g_futex_inited) {
        InitializeCriticalSection(&g_futex_cs);
        InitializeConditionVariable(&g_futex_cv);
        g_futex_inited = TRUE;
    }
}

static int futex_wait(volatile uint32_t *addr, uint32_t expected)
{
    futex_ensure_init();
    EnterCriticalSection(&g_futex_cs);
    while (*addr == expected) {
        SleepConditionVariableCS(&g_futex_cv, &g_futex_cs, INFINITE);
    }
    LeaveCriticalSection(&g_futex_cs);
    return 0;
}

static int futex_wake(volatile uint32_t *addr, int n)
{
    (void)addr; (void)n;
    futex_ensure_init();
    WakeAllConditionVariable(&g_futex_cv);
    return 0;
}

#else /* Linux */

static int futex_wait(volatile uint32_t *addr, uint32_t expected)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static int futex_wake(volatile uint32_t *addr, int n)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, n, NULL, NULL, 0);
}

#endif /* _WIN32 */

/* ── Ring buffer ──────────────────────────────────────────────── */

void ufta_ring_init(ufta_ring_t *ring)
{
    memset(ring, 0, sizeof(*ring));
    ring->head = 0;
    ring->tail = 0;
    ring->seq  = 0;
}

int ufta_ring_push(ufta_ring_t *ring, uint32_t page_index)
{
    uint32_t head = ring->head;
    uint32_t tail = ring->tail;

    /* Full? */
    if (head - tail >= UFTA_WORKER_RING_SIZE)
        return -1;

    uint32_t slot = head % UFTA_WORKER_RING_SIZE;
    ring->slots[slot].page_index = page_index;
    ring->slots[slot].seq        = ring->seq++;
    /* Release: ensure slot write is visible before head update */
    __sync_synchronize();
    ring->head = head + 1;
    return 0;
}

int ufta_ring_pop(ufta_ring_t *ring, ufta_fault_req_t *out)
{
    uint32_t tail = ring->tail;
    uint32_t head = ring->head;

    if (tail == head)
        return -1; /* empty */

    uint32_t slot = tail % UFTA_WORKER_RING_SIZE;
    *out = ring->slots[slot];
    /* Acquire: ensure we read the slot before advancing tail */
    __sync_synchronize();
    ring->tail = tail + 1;
    return 0;
}

/* ── Worker thread main loop ──────────────────────────────────── */

static void *worker_main(void *arg)
{
    ufta_worker_t *w = (ufta_worker_t *)arg;

    while (w->running) {
        /* Wait for work */
        pthread_mutex_lock(&w->notify_mutex);
        while (w->running && w->ring.head == w->ring.tail) {
            pthread_cond_wait(&w->notify_cond, &w->notify_mutex);
        }
        pthread_mutex_unlock(&w->notify_mutex);

        if (!w->running)
            break;

        /* Drain the ring buffer into a batch */
        uint32_t count = 0;
        ufta_fault_req_t req;
        while (count < UFTA_WORKER_RING_SIZE &&
               ufta_ring_pop(&w->ring, &req) == 0) {
            w->batch_idx[count++] = req.page_index;
        }

        if (count == 0)
            continue;

        /* Coalesce adjacent pages into larger blocks.
         * The batch_idx array is already sorted by page_index because
         * faults arrive roughly in address order; we still sort to be
         * safe and then merge contiguous runs. */
        for (uint32_t i = 1; i < count; i++) {
            uint32_t key = w->batch_idx[i];
            uint32_t j = i;
            while (j > 0 && w->batch_idx[j - 1] > key) {
                w->batch_idx[j] = w->batch_idx[j - 1];
                j--;
            }
            w->batch_idx[j] = key;
        }

        /* Invoke the load callback with the full batch. The callback
         * (pagefault layer) performs the actual transfer, applying
         * mprotect(RW) and coalescing contiguous pages internally. */
        if (w->load_fn) {
            int rc = w->load_fn(w->load_ctx, w->batch_idx, count);
            (void)rc;
        }

        /* Update stats */
        w->total_batches++;
        w->total_pages_processed += count;
        if (count > w->max_batch_pages)
            w->max_batch_pages = count;

        /* Wake all suspended faulting threads */
        ufta_worker_wake_all(w);
    }

    return NULL;
}

/* ── Worker lifecycle ─────────────────────────────────────────── */

int ufta_worker_start(ufta_worker_t *w, ufta_worker_load_fn load_fn,
                      void *ctx)
{
    if (!w || !load_fn)
        return -1;

    memset(w, 0, sizeof(*w));
    ufta_ring_init(&w->ring);
    w->load_fn  = load_fn;
    w->load_ctx = ctx;
    w->running  = true;

    pthread_mutex_init(&w->notify_mutex, NULL);
    pthread_cond_init(&w->notify_cond, NULL);

    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        w->running = false;
        pthread_mutex_destroy(&w->notify_mutex);
        pthread_cond_destroy(&w->notify_cond);
        return -1;
    }

    return 0;
}

void ufta_worker_stop(ufta_worker_t *w)
{
    if (!w || !w->running)
        return;

    w->running = false;
    ufta_worker_notify(w);
    pthread_join(w->thread, NULL);

    pthread_mutex_destroy(&w->notify_mutex);
    pthread_cond_destroy(&w->notify_cond);
}

void ufta_worker_notify(ufta_worker_t *w)
{
    pthread_mutex_lock(&w->notify_mutex);
    pthread_cond_signal(&w->notify_cond);
    pthread_mutex_unlock(&w->notify_mutex);
}

/* ── Suspension of faulting threads ───────────────────────────── */

void ufta_worker_wait(ufta_worker_t *w, uint32_t page_index)
{
    /* Register as a waiter using a per-slot futex word. We use the
     * page_index as the futex value so the worker can wake precisely
     * the threads whose pages have been resolved. */
    uint32_t slot = 0;
    for (uint32_t i = 0; i < UFTA_WORKER_MAX_WAITERS; i++) {
        if (w->waiters[i] == 0) {
            w->waiters[i] = page_index + 1; /* 0 = free slot */
            slot = i;
            break;
        }
    }

    /* Wait until the worker clears our slot (i.e. resolves the page) */
    while (w->waiters[slot] != 0) {
        futex_wait(&w->waiters[slot], w->waiters[slot]);
    }
}

void ufta_worker_wake_all(ufta_worker_t *w)
{
    for (uint32_t i = 0; i < UFTA_WORKER_MAX_WAITERS; i++) {
        if (w->waiters[i] != 0) {
            w->waiters[i] = 0;
            futex_wake(&w->waiters[i], 1);
            w->total_wakeups++;
        }
    }
}

/* ── Statistics ───────────────────────────────────────────────── */

void ufta_worker_stats_print(const ufta_worker_t *w, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;

    fprintf(f, "=== UFTA Worker Thread ===\n");
    fprintf(f, "  Batches processed : %llu\n",
            (unsigned long long)w->total_batches);
    fprintf(f, "  Pages processed   : %llu\n",
            (unsigned long long)w->total_pages_processed);
    fprintf(f, "  Bytes moved       : %llu\n",
            (unsigned long long)w->total_bytes_moved);
    fprintf(f, "  Max batch size    : %llu pages\n",
            (unsigned long long)w->max_batch_pages);
    fprintf(f, "  Wakeups           : %llu\n",
            (unsigned long long)w->total_wakeups);
    fclose(f);
}
