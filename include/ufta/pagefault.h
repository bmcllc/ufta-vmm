/*
 * UFTA-VMM — pagefault.h — Transparent page fault handler
 *
 * Uses SIGSEGV + mprotect(PROT_NONE) to intercept page accesses.
 * When a page is accessed that's been "evicted" to a lower tier,
 * the handler transparently loads it back into RAM.
 *
 * This is the key mechanism that makes the VMM transparent —
 * applications don't need to know which tier their data lives on.
 */

#ifndef UFTA_PAGEFAULT_H
#define UFTA_PAGEFAULT_H

#include "types.h"
#include "tier.h"
#include "worker.h"
#include "platform.h"

/* CUDA backend (only when compiled with nvcc) */
#ifdef HAVE_CUDA_BACKEND
#include "ufta/cuda_backend.h"
#define PF_HAS_CUDA 1
#else
#define PF_HAS_CUDA 0
#endif

#define PF_MAX_PAGES    65536
#define PF_BACKING_DIR  "/tmp/ufta-pf"

/* ── Page fault page state ────────────────────────────────────── */

typedef enum {
    PF_LOC_RAM     = 0,   /* page is in RAM (accessible)       */
    PF_LOC_VRAM    = 1,   /* page is in VRAM-sim (evicted)     */
    PF_LOC_FILE    = 2,   /* page is on disk (evicted)         */
    PF_LOC_MISSING = 3,   /* page was never allocated           */
    PF_LOC_VRAM_REAL = 4, /* page is in REAL GPU VRAM (evicted) */
} pf_location_t;

typedef struct {
    void          *vaddr;       /* virtual address (mmap'd)           */
    pf_location_t  location;    /* where the data currently lives     */
    uint32_t       page_index;  /* index in page array                */
    uint64_t       size;        /* page size                          */
    bool           dirty;       /* modified since last eviction       */
    bool           valid;       /* is this slot in use?               */

    /* Backing store paths */
    int            backing_fd;  /* memfd or file fd for this page     */
    off_t          backing_off; /* offset in backing file             */

    /* Statistics */
    uint64_t       access_count;
    uint64_t       fault_count; /* times this page was faulted in     */
    uint64_t       last_fault_ts;
} pf_page_t;

/* ── Page fault handler context ───────────────────────────────── */

typedef struct {
    /* Virtual memory region */
    void          *region_base;      /* base of mmap'd region         */
    size_t         region_size;      /* total region size             */
    uint32_t       page_size;        /* per-page size                 */
    uint32_t       page_count;       /* number of pages               */

    /* Page tracking */
    pf_page_t      pages[PF_MAX_PAGES];
    uint32_t       pages_active;     /* how many are valid             */

    /* Tier backends */
    int            vram_fd;          /* memfd for VRAM-sim            */
    int            file_fd;          /* file backend for FILE tier    */

#if PF_HAS_CUDA
    /* Real GPU VRAM backing */
    cuda_mem_t     vram_real;        /* real VRAM region (cudaMalloc) */
    bool           vram_real_ready;  /* is real VRAM backing active?  */
    gpu_info_t     gpu;              /* GPU info                      */
#endif

    /* Original signal handler */
#ifdef _WIN32
    void *old_veh_handle;   /* VEH handle for cleanup */
#else
    struct sigaction old_sa_segv;
#endif

    /* Statistics */
    uint64_t       total_faults;
    uint64_t       total_evictions;
    uint64_t       total_loads;
    uint64_t       total_stores;
    uint64_t       bytes_loaded;
    uint64_t       bytes_stored;

    /* Runtime state */
    bool           active;           /* handler installed?             */
    bool           fault_in_progress; /* re-entrancy guard            */

    /* ── Async worker thread (batching) ─────────────────────── */
    ufta_worker_t  worker;           /* worker thread + ring buffer   */
    bool           worker_started;   /* is the worker running?        */
} pf_context_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize the page fault handler */
int pf_init(pf_context_t *ctx, uint32_t page_size, uint32_t page_count);

/* Initialize with REAL GPU VRAM backing (requires CUDA) */
int pf_init_cuda(pf_context_t *ctx, uint32_t page_size, uint32_t page_count);

/* Cleanup */
void pf_destroy(pf_context_t *ctx);

/* Allocate a virtual page (returns pointer to accessible memory) */
void *pf_page_alloc(pf_context_t *ctx, uint32_t index);

/* Evict a page to a slower tier (marks PROT_NONE, saves to backend) */
int pf_page_evict(pf_context_t *ctx, uint32_t index, pf_location_t target);

/* Page in a page from its backend (restores PROT_READ|PROT_WRITE) */
int pf_page_load(pf_context_t *ctx, uint32_t index);

/* Write known pattern to a page (for testing) */
int pf_page_write_pattern(pf_context_t *ctx, uint32_t index, uint8_t val);

/* Verify pattern of a page (for testing) */
int pf_page_verify_pattern(pf_context_t *ctx, uint32_t index, uint8_t val);

/* Access a page by pointer (simulates app read — updates stats) */
static inline void pf_touch(pf_context_t *ctx, uint32_t index) {
    if (index < ctx->page_count && ctx->pages[index].valid) {
        ctx->pages[index].access_count++;
    }
}

/* Print handler stats */
void pf_stats_print(const pf_context_t *ctx, int fd);

#endif /* UFTA_PAGEFAULT_H */
