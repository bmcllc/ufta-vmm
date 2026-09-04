/*
 * UFTA-VMM — validate.h — Real-world validation
 *
 * Prova de conceito: medir latência/banda REAL de cada tier
 * e testar migração de dados reais entre RAM e VRAM.
 *
 * O objetivo é validar se a VRAM pode ser tratada como
 * memória virtual endereçável (o que ninguém conseguiu ainda).
 */

#ifndef UFTA_VALIDATE_H
#define UFTA_VALIDATE_H

#include "types.h"
#include "tier.h"
#include "page.h"

/* ── Benchmark result ─────────────────────────────────────────── */

typedef struct {
    tier_id_t tier_id;
    char      tier_name[32];

    real_t    bw_read;        /* bytes/s read  */
    real_t    bw_write;       /* bytes/s write */
    real_t    latency_read;   /* seconds       */
    real_t    latency_write;  /* seconds       */

    uint64_t  bytes_tested;
    real_t    duration_s;
} bench_result_t;

/* ── Real memory backend (RAM via /dev/shm or malloc) ─────────── */

typedef struct {
    void     *ptr;            /* mapped memory        */
    size_t    size;           /* total size           */
    char      name[32];       /* tier name            */
    bool      is_mapped;      /* mmap'd?              */
    int       fd;             /* fd if mmap'd         */
} real_mem_t;

/* ── Migration test result ────────────────────────────────────── */

typedef struct {
    uint64_t  pages_migrated;
    uint64_t  bytes_moved;
    uint64_t  pages_verified;   /* integrity checks passed */
    uint64_t  pages_failed;     /* integrity checks failed */
    real_t    total_time_s;
    real_t    effective_bw;     /* bytes/s actually achieved */
    bool      all_ok;
} migrate_test_result_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Allocate a real memory region (RAM via malloc, or /dev/shm) */
int real_mem_alloc(real_mem_t *rm, const char *name, size_t size);

/* Free a real memory region */
void real_mem_free(real_mem_t *rm);

/* Benchmark a real memory region: measure read/write bandwidth */
bench_result_t bench_memory(const real_mem_t *rm, size_t block_size,
                            int iterations);

/* Benchmark a file-backed region (disk tier) */
bench_result_t bench_file(const char *path, size_t block_size, int iterations);

/* Benchmark a memfd region (simulated VRAM via shared memory) */
bench_result_t bench_memfd(const char *name, size_t size, size_t block_size,
                           int iterations);

/* Run full benchmark on all tiers, updating tier registry */
void validate_benchmark_all(tier_registry_t *reg);

/* Migration test: allocate pages in RAM, migrate to VRAM-sim,
 * verify data integrity, migrate back */
migrate_test_result_t validate_migration_test(tier_registry_t *reg,
                                              size_t page_size,
                                              uint32_t num_pages);

/* Print benchmark results */
void bench_result_print(const bench_result_t *r, int fd);

/* Print migration test results */
void migrate_test_result_print(const migrate_test_result_t *r, int fd);

#endif /* UFTA_VALIDATE_H */
