/*
 * UFTA-VMM — worker.h — Asynchronous page-fault worker thread
 *
 * Solves the three bottlenecks of the synchronous SIGSEGV handler:
 *
 *   1. Async-signal-safety: CUDA/OS calls run on the worker thread,
 *      never inside the signal context (avoids deadlock/UB).
 *   2. Context-switch cost: the signal handler only records the fault
 *      in a lock-free ring buffer and suspends the faulting thread via
 *      futex — the heavy lifting happens off the hot path.
 *   3. Granularity: the worker coalesces adjacent 4 KB faults into
 *      larger PCIe-friendly blocks (64 KB – 2 MB) and issues a single
 *      batched transfer.
 *
 * Flow:
 *   App thread  → SIGSEGV → handler enqueues fault + futex_wait
 *   Worker      → drains ring buffer, batches adjacent pages,
 *                 performs cudaMemcpyAsync / pread, mprotect(RW),
 *                 futex_wake the waiting threads.
 */

#ifndef UFTA_WORKER_H
#define UFTA_WORKER_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* ── Configuration ────────────────────────────────────────────── */

#define UFTA_WORKER_RING_SIZE   4096   /* lock-free ring capacity   */
#define UFTA_WORKER_BATCH_MIN   (64 * 1024)   /* 64 KB  min batch   */
#define UFTA_WORKER_BATCH_MAX   (2 * 1024 * 1024) /* 2 MB max batch */
#define UFTA_WORKER_MAX_WAITERS 256   /* max suspended threads      */

/* ── Fault request (single page) ──────────────────────────────── */

typedef struct {
    uint32_t page_index;   /* index into pf_context_t->pages[] */
    uint32_t seq;          /* monotonically increasing seq      */
} ufta_fault_req_t;

/* ── Lock-free ring buffer (SPSC: signal handler → worker) ─────
 * Single producer (the signal handler, one thread at a time due to
 * the re-entrancy guard) and single consumer (the worker thread).
 * Uses a simple seq-based scheme with acquire/release ordering.
 */
typedef struct {
    ufta_fault_req_t slots[UFTA_WORKER_RING_SIZE];
    volatile uint32_t head;   /* producer index (signal handler) */
    volatile uint32_t tail;   /* consumer index (worker)         */
    volatile uint32_t seq;    /* global fault sequence number    */
} ufta_ring_t;

/* ── Worker thread context ────────────────────────────────────── */

typedef struct ufta_worker ufta_worker_t;

/* Callback invoked by the worker to actually resolve a batch of
 * faults. Implemented by the page-fault layer (pagefault.c).
 *
 *   ctx      — opaque user context (pf_context_t*)
 *   idx      — array of page indices to load (contiguous batch)
 *   count    — number of pages in the batch
 *
 * Returns 0 on success. The worker applies mprotect(RW) and wakes
 * the waiting threads after a successful batch.
 */
typedef int (*ufta_worker_load_fn)(void *ctx, const uint32_t *idx,
                                   uint32_t count);

struct ufta_worker {
    ufta_ring_t  ring;

    pthread_t    thread;
    bool         running;

    /* Synchronization: worker notifies handler it has work */
    pthread_mutex_t notify_mutex;
    pthread_cond_t  notify_cond;

    /* Suspended faulting threads (futex-based wait) */
    volatile uint32_t waiters[UFTA_WORKER_MAX_WAITERS];
    volatile uint32_t num_waiters;

    /* Batch scratch buffer */
    uint32_t batch_idx[UFTA_WORKER_RING_SIZE];

    /* Callback + user context */
    ufta_worker_load_fn load_fn;
    void               *load_ctx;

    /* Statistics */
    uint64_t total_batches;
    uint64_t total_pages_processed;
    uint64_t total_bytes_moved;
    uint64_t max_batch_pages;
    uint64_t total_wakeups;
};

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize ring buffer (call before starting worker) */
void ufta_ring_init(ufta_ring_t *ring);

/* Producer: enqueue a fault request. Returns 0 on success, -1 if full. */
int ufta_ring_push(ufta_ring_t *ring, uint32_t page_index);

/* Consumer: pop a fault request. Returns 0 on success, -1 if empty. */
int ufta_ring_pop(ufta_ring_t *ring, ufta_fault_req_t *out);

/* Start the worker thread. load_fn is called to resolve batches. */
int ufta_worker_start(ufta_worker_t *w, ufta_worker_load_fn load_fn,
                      void *ctx);

/* Stop and join the worker thread. */
void ufta_worker_stop(ufta_worker_t *w);

/* Signal the worker that new work is available. */
void ufta_worker_notify(ufta_worker_t *w);

/* Suspend the calling thread until the worker resolves its fault.
 * Returns when the worker has processed the given page index. */
void ufta_worker_wait(ufta_worker_t *w, uint32_t page_index);

/* Wake all suspended waiters (called by worker after a batch). */
void ufta_worker_wake_all(ufta_worker_t *w);

/* Print worker statistics. */
void ufta_worker_stats_print(const ufta_worker_t *w, int fd);

#endif /* UFTA_WORKER_H */
