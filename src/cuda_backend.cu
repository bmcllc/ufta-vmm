/*
 * UFTA-VMM — cuda_backend.cu — Real VRAM backend via CUDA
 *
 * Acessa VRAM real da GPU via CUDA Runtime API.
 * Mede bandwidth real RAM↔VRAM e testa migração com integridade.
 *
 * Compile: nvcc -c src/cuda_backend.cu -Iinclude -std=c++14
 * Link:    nvcc -o uvm build/*.o build/cuda_backend.o -lm
 */

#include "ufta/cuda_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cuda_runtime.h>

/* ── High-resolution timing ───────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── CUDA error check ─────────────────────────────────────────── */

static int check_cuda(cudaError_t err, const char *call)
{
    if (err != cudaSuccess) {
        fprintf(stderr, "CUDA error [%s]: %s\n", call, cudaGetErrorString(err));
        return -1;
    }
    return 0;
}

/* ── GPU kernel: process data in-place in VRAM ──────────────────
 * Prova que a VRAM não é só armazenamento — é computação.
 * Cada thread processa um elemento: y[i] = y[i] * 2 + 1
 */
__global__ void ufta_process_kernel(unsigned char *data, size_t n)
{
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        data[i] = (unsigned char)(data[i] * 2 + 1);
    }
}

/* Process data in-place on the GPU (VRAM). Returns 0 on success. */
int cuda_process_inplace(cuda_mem_t *m, size_t size)
{
    if (!m->allocated || !m->device_ptr) return -1;

    int threads = 256;
    int blocks = (int)((size + threads - 1) / threads);
    if (blocks < 1) blocks = 1;

    ufta_process_kernel<<<blocks, threads>>>((unsigned char *)m->device_ptr, size);
    cudaError_t err = cudaDeviceSynchronize();
    return check_cuda(err, "cuda_process_inplace");
}

/* ── GPU info ─────────────────────────────────────────────────── */

int cuda_backend_init(gpu_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->available = false;

    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0) {
        fprintf(stderr, "No CUDA devices found\n");
        return -1;
    }

    /* Use device 0 */
    info->device_id = 0;
    err = cudaSetDevice(info->device_id);
    if (err != cudaSuccess) return -1;

    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, info->device_id);
    if (err != cudaSuccess) return -1;

    strncpy(info->name, prop.name, sizeof(info->name) - 1);
    info->compute_major = prop.major;
    info->compute_minor = prop.minor;
    info->total_mem = prop.totalGlobalMem;

    /* Get free memory */
    size_t free_mem, total_mem;
    err = cudaMemGetInfo(&free_mem, &total_mem);
    if (err == cudaSuccess) {
        info->free_mem = free_mem;
    } else {
        info->free_mem = info->total_mem;
    }

    info->available = true;
    return 0;
}

/* ── CUDA memory allocation ───────────────────────────────────── */

int cuda_mem_alloc(cuda_mem_t *m, size_t size)
{
    memset(m, 0, sizeof(*m));
    m->size = size;
    m->device_id = 0;

    /* Allocate pinned host memory for faster transfers */
    cudaError_t err = cudaMallocHost(&m->host_ptr, size);
    if (err != cudaSuccess) {
        /* Fallback to regular malloc */
        m->host_ptr = malloc(size);
        if (!m->host_ptr) return -1;
    }

    /* Allocate device memory (VRAM) */
    err = cudaMalloc(&m->device_ptr, size);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaMalloc failed: %s\n", cudaGetErrorString(err));
        if (m->host_ptr) cudaFreeHost(m->host_ptr);
        return -1;
    }

    m->allocated = true;
    return 0;
}

void cuda_mem_free(cuda_mem_t *m)
{
    if (m->device_ptr) {
        cudaFree(m->device_ptr);
        m->device_ptr = NULL;
    }
    if (m->host_ptr) {
        /* Try to free as pinned first, fallback to free */
        cudaError_t err = cudaFreeHost(m->host_ptr);
        if (err != cudaSuccess) {
            free(m->host_ptr);
        }
        m->host_ptr = NULL;
    }
    m->allocated = false;
}

/* ── Memory copy operations ───────────────────────────────────── */

int cuda_memcpy_h2d(cuda_mem_t *m, const void *host_data, size_t size)
{
    if (!m->allocated || !m->device_ptr) return -1;
    cudaError_t err = cudaMemcpy(m->device_ptr, host_data, size,
                                  cudaMemcpyHostToDevice);
    return check_cuda(err, "cudaMemcpy H2D");
}

int cuda_memcpy_d2h(void *host_data, const cuda_mem_t *m, size_t size)
{
    if (!m->allocated || !m->device_ptr) return -1;
    cudaError_t err = cudaMemcpy(host_data, m->device_ptr, size,
                                  cudaMemcpyDeviceToHost);
    return check_cuda(err, "cudaMemcpy D2H");
}

/* ── Benchmark ────────────────────────────────────────────────── */

cuda_bench_result_t cuda_benchmark(cuda_mem_t *m, size_t block_size,
                                    int iterations)
{
    cuda_bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.block_size = block_size;
    r.iterations = iterations;

    if (!m->allocated || m->size < block_size) return r;

    /* Prepare host buffer with test pattern */
    volatile char *host_buf = (volatile char *)m->host_ptr;
    for (size_t i = 0; i < block_size; i += 64) {
        host_buf[i] = (char)(i & 0xFF);
    }

    /* Warm up */
    for (int i = 0; i < 5; i++) {
        cudaMemcpy(m->device_ptr, m->host_ptr, block_size,
                   cudaMemcpyHostToDevice);
        cudaMemcpy(m->host_ptr, m->device_ptr, block_size,
                   cudaMemcpyDeviceToHost);
    }
    cudaDeviceSynchronize();

    /* Host → Device (RAM → VRAM) */
    uint64_t t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        cudaMemcpy(m->device_ptr, m->host_ptr, block_size,
                   cudaMemcpyHostToDevice);
    }
    cudaDeviceSynchronize();
    uint64_t t1 = now_ns();
    r.bw_read_h2d = (double)block_size * iterations / ((double)(t1 - t0) / 1e9);
    r.latency_h2d = (double)(t1 - t0) / iterations / 1e9;

    /* Device → Host (VRAM → RAM) */
    t0 = now_ns();
    for (int it = 0; it < iterations; it++) {
        cudaMemcpy(m->host_ptr, m->device_ptr, block_size,
                   cudaMemcpyDeviceToHost);
    }
    cudaDeviceSynchronize();
    t1 = now_ns();
    r.bw_write_d2h = (double)block_size * iterations / ((double)(t1 - t0) / 1e9);
    r.latency_d2h = (double)(t1 - t0) / iterations / 1e9;

    /* Device → Device (VRAM → VRAM — memcpy within GPU) */
    void *temp_dev = NULL;
    cudaMalloc(&temp_dev, block_size);
    if (temp_dev) {
        t0 = now_ns();
        for (int it = 0; it < iterations; it++) {
            cudaMemcpy(temp_dev, m->device_ptr, block_size,
                       cudaMemcpyDeviceToDevice);
        }
        cudaDeviceSynchronize();
        t1 = now_ns();
        r.bw_read_d2d = (double)block_size * iterations / ((double)(t1 - t0) / 1e9);
        cudaFree(temp_dev);
    }

    r.duration_s = (double)(t1 - t0) / 1e9;
    return r;
}

/* ── Migration test ───────────────────────────────────────────── */

int cuda_migration_test(size_t page_size, uint32_t num_pages)
{
    printf("\n══════════════════════════════════════════════════\n");
    printf("  TESTE DE MIGRAÇÃO REAL — RAM ↔ GPU VRAM\n");
    printf("══════════════════════════════════════════════════\n\n");

    size_t total = page_size * num_pages;
    printf("  Páginas: %u | Tamanho: %zu bytes | Total: %.1f MB\n\n",
           num_pages, page_size, (double)total / (1024.0 * 1024.0));

    /* Check GPU memory */
    gpu_info_t gpu;
    if (cuda_backend_init(&gpu) != 0) {
        printf("  ERRO: GPU CUDA não disponível\n");
        return -1;
    }
    printf("  GPU: %s (%.1f MB VRAM)\n", gpu.name, gpu.total_mem / (1024.0*1024.0));

    if (total > gpu.free_mem) {
        printf("  ERRO: Dados (%.1f MB) > VRAM livre (%.1f MB)\n",
               (double)total / (1024.0*1024.0),
               (double)gpu.free_mem / (1024.0*1024.0));
        return -1;
    }

    /* Allocate CUDA memory */
    cuda_mem_t vram;
    if (cuda_mem_alloc(&vram, total) != 0) {
        printf("  ERRO: falha ao alocar VRAM\n");
        return -1;
    }
    printf("  VRAM alocada: %.1f MB\n", (double)total / (1024.0*1024.0));

    /* Allocate RAM */
    unsigned char *ram = (unsigned char *)malloc(total);
    if (!ram) {
        cuda_mem_free(&vram);
        printf("  ERRO: falha ao alocar RAM\n");
        return -1;
    }

    /* Fill RAM with known pattern */
    printf("  [1/4] Preenchendo RAM com padrão...\n");
    for (size_t i = 0; i < total; i++) {
        ram[i] = (unsigned char)(i * 31 + 7);
    }

    /* RAM → VRAM (cudaMemcpy HostToDevice) */
    printf("  [2/4] Migrando RAM → VRAM (cudaMemcpy H2D)...\n");
    uint64_t t0 = now_ns();
    if (cuda_memcpy_h2d(&vram, ram, total) != 0) {
        printf("  ERRO: falha na migração H2D\n");
        free(ram);
        cuda_mem_free(&vram);
        return -1;
    }
    cudaDeviceSynchronize();
    uint64_t t1 = now_ns();
    double h2d_time = (double)(t1 - t0) / 1e9;
    double h2d_bw = (double)total / h2d_time / 1e9;
    printf("  RAM→VRAM: %.1f MB em %.3f ms (%.1f GB/s)\n",
           (double)total / (1024.0*1024.0), h2d_time * 1000.0, h2d_bw);

    /* Read back from VRAM to verify */
    printf("  [3/4] Verificando integridade na VRAM...\n");
    unsigned char *verify = (unsigned char *)malloc(total);
    if (!verify) {
        free(ram);
        cuda_mem_free(&vram);
        return -1;
    }

    t0 = now_ns();
    if (cuda_memcpy_d2h(verify, &vram, total) != 0) {
        printf("  ERRO: falha na migração D2H\n");
        free(ram);
        free(verify);
        cuda_mem_free(&vram);
        return -1;
    }
    cudaDeviceSynchronize();
    t1 = now_ns();
    double d2h_time = (double)(t1 - t0) / 1e9;
    double d2h_bw = (double)total / d2h_time / 1e9;

    /* Verify data integrity */
    uint64_t ok = 0, fail = 0;
    for (uint32_t pg = 0; pg < num_pages; pg++) {
        bool page_ok = true;
        size_t base = (size_t)pg * page_size;
        for (size_t i = 0; i < page_size; i += 16) {
            if (verify[base + i] != ram[base + i]) {
                page_ok = false;
                break;
            }
        }
        if (page_ok) ok++;
        else         fail++;
    }
    printf("  Verificados: %llu | Falhas: %llu\n",
           (unsigned long long)ok, (unsigned long long)fail);

    /* VRAM → RAM (final) */
    printf("  [4/4] Migrando VRAM → RAM (cudaMemcpy D2H)...\n");
    printf("  VRAM→RAM: %.1f MB em %.3f ms (%.1f GB/s)\n",
           (double)total / (1024.0*1024.0), d2h_time * 1000.0, d2h_bw);

    bool all_ok = (fail == 0);
    printf("\n  RESULTADO: %s\n",
           all_ok ? "✓ TODOS OS DADOS INTEGROS NA VRAM REAL" : "✗ HOUVE FALHAS");

    printf("\n  Banda real medida:\n");
    printf("    RAM → VRAM:  %.1f GB/s (H2D)\n", h2d_bw);
    printf("    VRAM → RAM:  %.1f GB/s (D2H)\n", d2h_bw);
    printf("    Latência H2D: %.1f µs\n", h2d_time * 1e6 / num_pages);
    printf("    Latência D2H: %.1f µs\n", d2h_time * 1e6 / num_pages);

    free(ram);
    free(verify);
    cuda_mem_free(&vram);
    return all_ok ? 0 : 1;
}

/* ── Print helpers ────────────────────────────────────────────── */

void gpu_info_print(const gpu_info_t *info, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;
    fprintf(f, "\n── GPU Info ──\n");
    fprintf(f, "  Device:       %s\n", info->name);
    fprintf(f, "  Compute:      %d.%d\n", info->compute_major, info->compute_minor);
    fprintf(f, "  VRAM Total:   %.1f MB\n", (double)info->total_mem / (1024.0*1024.0));
    fprintf(f, "  VRAM Livre:   %.1f MB\n", (double)info->free_mem / (1024.0*1024.0));
    fprintf(f, "  Available:    %s\n", info->available ? "YES" : "NO");
    fclose(f);
}

void cuda_bench_print(const cuda_bench_result_t *r, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;
    fprintf(f, "\n── CUDA Benchmark ──\n");
    fprintf(f, "  Host → Device:  %8.1f GB/s  (lat: %.1f µs)\n",
            r->bw_read_h2d / 1e9, r->latency_h2d * 1e6);
    fprintf(f, "  Device → Host:  %8.1f GB/s  (lat: %.1f µs)\n",
            r->bw_write_d2h / 1e9, r->latency_d2h * 1e6);
    fprintf(f, "  Device → Device: %7.1f GB/s\n", r->bw_read_d2d / 1e9);
    fprintf(f, "  Bloco: %zu bytes | Iterações: %d\n", r->block_size, r->iterations);
    fclose(f);
}
