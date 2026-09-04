/*
 * UFTA-VMM — cuda_backend.h — Real VRAM backend via CUDA
 *
 * Acessa VRAM real da GPU via CUDA Runtime API.
 * GTX 1650: ~128 GB/s bandwidth, 4 GB VRAM.
 *
 * Compile com: nvcc -c src/cuda_backend.cu -Iinclude
 */

#ifndef UFTA_CUDA_BACKEND_H
#define UFTA_CUDA_BACKEND_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>

/* CUDA runtime (for cudaDeviceSynchronize etc.) */
#include <cuda_runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── GPU info ─────────────────────────────────────────────────── */

typedef struct {
    int       device_id;
    char      name[256];
    uint64_t  total_mem;       /* total VRAM bytes */
    uint64_t  free_mem;        /* free VRAM bytes  */
    int       compute_major;
    int       compute_minor;
    bool      available;
} gpu_info_t;

/* ── CUDA memory handle ───────────────────────────────────────── */

typedef struct {
    void     *device_ptr;      /* pointer on GPU     */
    void     *host_ptr;        /* pinned host memory */
    size_t    size;
    int       device_id;
    bool      allocated;
} cuda_mem_t;

/* ── Benchmark result from GPU ────────────────────────────────── */

typedef struct {
    double    bw_read_h2d;     /* host → device (RAM → VRAM) */
    double    bw_write_d2h;    /* device → host (VRAM → RAM) */
    double    bw_read_d2d;     /* device → device (VRAM → VRAM) */
    double    bw_write_p2p;    /* peer-to-peer if available  */
    double    latency_h2d;
    double    latency_d2h;
    size_t    block_size;
    int       iterations;
    double    duration_s;
} cuda_bench_result_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize CUDA and get GPU info */
int cuda_backend_init(gpu_info_t *info);

/* Allocate GPU memory (VRAM) */
int cuda_mem_alloc(cuda_mem_t *m, size_t size);

/* Free GPU memory */
void cuda_mem_free(cuda_mem_t *m);

/* Copy data: host → device (RAM → VRAM) */
int cuda_memcpy_h2d(cuda_mem_t *m, const void *host_data, size_t size);

/* Copy data: device → host (VRAM → RAM) */
int cuda_memcpy_d2h(void *host_data, const cuda_mem_t *m, size_t size);

/* Process data in-place on the GPU (VRAM) — proves VRAM is compute */
int cuda_process_inplace(cuda_mem_t *m, size_t size);

/* Benchmark GPU memory bandwidth */
cuda_bench_result_t cuda_benchmark(cuda_mem_t *m, size_t block_size,
                                    int iterations);

/* Full migration test: allocate in RAM, copy to VRAM, verify, copy back */
int cuda_migration_test(size_t page_size, uint32_t num_pages);

/* Print GPU info */
void gpu_info_print(const gpu_info_t *info, int fd);

/* Print CUDA benchmark results */
void cuda_bench_print(const cuda_bench_result_t *r, int fd);

#ifdef __cplusplus
}
#endif

#endif /* UFTA_CUDA_BACKEND_H */
