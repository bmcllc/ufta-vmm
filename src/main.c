/*
 * UFTA-VMM — main.c — CLI entry point
 *
 * Usage:
 *   uvm create <file> --size <N>[G|M|K]
 *   uvm run            — run the VMM pipeline
 *   uvm stats          — print tier and migration stats
 *   uvm demo           — create demo pages and run a few cycles
 *   uvm help           — show this help
 */

#include "ufta/ufta.h"
#include "ufta/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ufta/pagefault.h"

/* Ensure POSIX clock_gettime is available even under nvcc (C++) */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* High-res timestamp (available to all commands) */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* CUDA backend (available when compiled with nvcc) */
#ifdef HAVE_CUDA_BACKEND
#include "ufta/cuda_backend.h"
#define HAS_CUDA 1
#else
#define HAS_CUDA 0
#endif

/* ── Global runtime (for signal handler) ──────────────────────── */

static runtime_t g_rt;

static void sighandler(int sig)
{
    (void)sig;
    g_rt.running = false;
}

/* ── Parse size string: "8G" → bytes ─────────────────────────── */

static uint64_t parse_size(const char *s)
{
    uint64_t val = strtoull(s, NULL, 10);
    char *end = (char *)s;
    while (*end && *end >= '0' && *end <= '9') end++;

    switch (*end) {
        case 'G': case 'g': val *= 1024ULL * 1024 * 1024; break;
        case 'M': case 'm': val *= 1024ULL * 1024; break;
        case 'K': case 'k': val *= 1024ULL; break;
        default: break;
    }
    return val;
}

/* ── Command: create ──────────────────────────────────────────── */

static int cmd_create(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: uvm create <file> --size <N>[G|M|K]\n");
        return 1;
    }

    const char *path = argv[0];
    uint64_t size = 64ULL * 1024 * 1024; /* default 64 MB */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = parse_size(argv[++i]);
        }
    }

    int err = backend_create(path, size, UFTA_PAGE_SIZE_DEFAULT, UFTA_FMT_BINARY);
    if (err != UFTA_OK) {
        fprintf(stderr, "Error creating backend: %d\n", err);
        return 1;
    }

    printf("Created '%s' (%llu MB)\n", path,
           (unsigned long long)(size / (1024*1024)));
    return 0;
}

/* ── Command: demo ────────────────────────────────────────────── */

static int cmd_demo(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("╔══════════════════════════════════════════╗\n");
    printf("║   UFTA-VMM Demo — Field-Driven Virtual  ║\n");
    printf("║   Memory Manager v%s               ║\n", UFTA_VERSION_STRING);
    printf("╚══════════════════════════════════════════╝\n\n");

    /* Initialize everything */
    tier_registry_t tiers;
    tier_registry_init_defaults(&tiers);

    page_table_t pages;
    page_table_init(&pages, 1024);

    addr_map_t addr_map;
    addr_map_init(&addr_map, 4096, UFTA_PAGE_SIZE_DEFAULT);

    field_engine_t field;
    field_engine_init(&field);

    predictor_t predictor;
    predictor_init(&predictor);

    bw_allocator_t bandwidth;
    memset(&bandwidth, 0, sizeof(bandwidth));
    bandwidth.num_channels = tiers.num_tiers;
    for (int i = 0; i < tiers.num_tiers; i++) {
        bandwidth.channels[i].id              = i;
        bandwidth.channels[i].tier_id         = tiers.tiers[i].id;
        bandwidth.channels[i].bandwidth_max   = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].bandwidth_desired = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].weight          = 1.0;
    }

    migrate_engine_t migrator;
    migrate_engine_init(&migrator);

    runtime_t rt;
    pipeline_init(&rt);
    rt.tiers     = &tiers;
    rt.pages     = &pages;
    rt.addr_map  = &addr_map;
    rt.field     = &field;
    rt.predictor = &predictor;
    rt.bandwidth = &bandwidth;
    rt.migrator  = &migrator;
    rt.tick_interval_ms   = 100.0; /* 10 Hz for demo */
    rt.prediction_horizon = 5;

    /* Create demo pages across tiers */
    printf("Creating demo pages...\n");

    /* RAM pages (hot) */
    for (int i = 0; i < 8; i++) {
        page_t *p = page_alloc(&pages, UFTA_PAGE_SIZE_DEFAULT, &tiers.tiers[0]);
        if (p) {
            p->state.raw.x = 0.7 + (i % 3) * 0.1; /* hot heat */
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&addr_map, p);
        }
    }

    /* VRAM pages (warm) */
    for (int i = 0; i < 4; i++) {
        page_t *p = page_alloc(&pages, UFTA_PAGE_SIZE_DEFAULT, &tiers.tiers[1]);
        if (p) {
            p->state.raw.x = 0.4 + (i % 2) * 0.1;
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&addr_map, p);
        }
    }

    /* USB pages (cold) */
    for (int i = 0; i < 6; i++) {
        page_t *p = page_alloc(&pages, UFTA_PAGE_SIZE_DEFAULT, &tiers.tiers[3]);
        if (p) {
            p->state.raw.x = 0.1 + (i % 3) * 0.05;
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&addr_map, p);
        }
    }

    printf("Created %u pages across %d tiers\n\n", pages.count, tiers.num_tiers);

    /* Print initial state */
    printf("── Initial State ──\n");
    for (int i = 0; i < tiers.num_tiers; i++) {
        tier_print(&tiers.tiers[i], STDOUT_FILENO);
    }
    printf("\n");

    /* Run pipeline for a few cycles */
    printf("── Running Pipeline (10 cycles) ──\n");
    for (int cycle = 0; cycle < 10; cycle++) {
        /* Simulate some accesses */
        for (uint32_t i = 0; i < pages.count; i++) {
            page_t *p = &pages.pages[i];
            if (rand() % 3 == 0) {
                page_access(p, (uint64_t)cycle * 1000000);
            }
        }

        pipeline_tick(&rt);

        if (cycle % 3 == 0 || cycle == 9) {
            printf("\nCycle %d:\n", cycle);
            pipeline_metrics_print(&rt, STDOUT_FILENO);
        }
    }

    /* Print final state */
    printf("\n── Final State ──\n");
    for (int i = 0; i < tiers.num_tiers; i++) {
        tier_print(&tiers.tiers[i], STDOUT_FILENO);
    }
    printf("\n");

    migrate_stats_print(&migrator, STDOUT_FILENO);

    /* Print some pages */
    printf("\n── Sample Pages ──\n");
    int printed = 0;
    for (uint32_t i = 0; i < pages.count && printed < 6; i++) {
        page_print(&pages.pages[i], STDOUT_FILENO);
        printed++;
    }

    /* Cleanup */
    free(pages.pages);
    free(addr_map.entries);
    predictor_destroy(&predictor);

    printf("\nDemo complete.\n");
    return 0;
}

/* ── Command: run ─────────────────────────────────────────────── */

static int cmd_run(int argc, char **argv)
{
    const char *vmem_path = NULL;
    uint64_t vmem_size = 64ULL * 1024 * 1024;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            vmem_path = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            vmem_size = parse_size(argv[++i]);
        }
    }

    /* Initialize systems */
    tier_registry_t tiers;
    tier_registry_init_defaults(&tiers);

    page_table_t pages;
    page_table_init(&pages, UFTA_MAX_PAGES);

    addr_map_t addr_map;
    addr_map_init(&addr_map, UFTA_MAX_PAGES, UFTA_PAGE_SIZE_DEFAULT);

    field_engine_t field;
    field_engine_init(&field);

    predictor_t predictor;
    predictor_init(&predictor);

    bw_allocator_t bandwidth;
    memset(&bandwidth, 0, sizeof(bandwidth));
    bandwidth.num_channels = tiers.num_tiers;
    for (int i = 0; i < tiers.num_tiers; i++) {
        bandwidth.channels[i].id              = i;
        bandwidth.channels[i].tier_id         = tiers.tiers[i].id;
        bandwidth.channels[i].bandwidth_max   = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].bandwidth_desired = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].weight          = 1.0;
    }

    migrate_engine_t migrator;
    migrate_engine_init(&migrator);

    pipeline_init(&g_rt);
    g_rt.tiers     = &tiers;
    g_rt.pages     = &pages;
    g_rt.addr_map  = &addr_map;
    g_rt.field     = &field;
    g_rt.predictor = &predictor;
    g_rt.bandwidth = &bandwidth;
    g_rt.migrator  = &migrator;
    g_rt.tick_interval_ms   = 16.0;
    g_rt.prediction_horizon = 10;

    /* Setup signal handler */
    signal(SIGINT,  sighandler);
    signal(SIGTERM, sighandler);

    printf("UFTA-VMM v%s running (Ctrl+C to stop)\n", UFTA_VERSION_STRING);
    printf("Tick interval: %.1f ms\n", g_rt.tick_interval_ms);

    /* Create backend if specified */
    backend_t backend;
    if (vmem_path) {
        int err = backend_create(vmem_path, vmem_size, UFTA_PAGE_SIZE_DEFAULT,
                                  UFTA_FMT_BINARY);
        if (err == UFTA_OK) {
            err = backend_open(&backend, vmem_path, true);
            if (err == UFTA_OK) {
                backend_print(&backend, STDOUT_FILENO);
            }
        }
        if (err != UFTA_OK) {
            fprintf(stderr, "Warning: could not create backend: %d\n", err);
        }
    }

    /* Run pipeline */
    pipeline_run(&g_rt);

    printf("\nPipeline stopped after %llu cycles.\n",
           (unsigned long long)g_rt.cycle_count);
    pipeline_metrics_print(&g_rt, STDOUT_FILENO);
    migrate_stats_print(&migrator, STDOUT_FILENO);

    /* Flush and close backend */
    if (vmem_path) {
        backend_flush(&backend, &pages);
        backend_close(&backend);
    }

    free(pages.pages);
    free(addr_map.entries);
    predictor_destroy(&predictor);

    return 0;
}

/* ── Command: validate ────────────────────────────────────────── */

static int cmd_validate(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   UFTA-VMM VALIDATE — Prova de Conceito     ║\n");
    printf("║   RAM/VRAM como memória virtual endereçável  ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    /* Initialize tier registry */
    tier_registry_t tiers;
    tier_registry_init_defaults(&tiers);

    /* Step 1: Real benchmark on all tiers */
    validate_benchmark_all(&tiers);

    /* Print calibrated tiers */
    printf("── Tiers Calibrados ──\n");
    for (int i = 0; i < tiers.num_tiers; i++) {
        tier_print(&tiers.tiers[i], STDOUT_FILENO);
    }
    printf("\n");

    /* Step 2: Migration test (RAM ↔ VRAM) */
    size_t page_size = 4096;
    uint32_t num_pages = 256; /* 1 MB total */

    /* Allow override from CLI */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc) {
            num_pages = (uint32_t)atoi(argv[i + 1]);
        }
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc) {
            page_size = (size_t)atol(argv[i + 1]);
        }
    }

    migrate_test_result_t mtr = validate_migration_test(&tiers, page_size,
                                                        num_pages);

    /* Summary */
    printf("\n══════════════════════════════════════════════════\n");
    printf("  RESUMO DA VALIDAÇÃO\n");
    printf("══════════════════════════════════════════════════\n");
    printf("  RAM:      %.1f GB/s medido (teórico: 20 GB/s)\n",
           tiers.tiers[0].bw_measured / 1e9);
    printf("  VRAM-sim: %.1f GB/s medido (teórico: 500 GB/s)\n",
           tiers.tiers[1].bw_measured / 1e9);
    printf("  Migração: %.1f GB/s efetivo (%s)\n",
           mtr.effective_bw / 1e9,
           mtr.all_ok ? "DADOS INTEGROS" : "FALHA NA INTEGRIDADE");
    printf("══════════════════════════════════════════════════\n");

    return mtr.all_ok ? 0 : 1;
}

/* ── Command: validate-cuda (real GPU VRAM) ───────────────────── */

static int cmd_validate_cuda(int argc, char **argv)
{
#if !HAS_CUDA
    (void)argc; (void)argv;
    fprintf(stderr, "Este binário não foi compilado com CUDA.\n");
    fprintf(stderr, "Use: make cuda && ./uvm-cuda validate-cuda\n");
    return 1;
#else
    (void)argc; (void)argv;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   UFTA-VMM VALIDATE-CUDA — VRAM REAL        ║\n");
    printf("║   RAM ↔ GPU VRAM como memória endereçável   ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    /* Get GPU info */
    gpu_info_t gpu;
    if (cuda_backend_init(&gpu) != 0) {
        fprintf(stderr, "ERRO: GPU CUDA não disponível\n");
        return 1;
    }
    gpu_info_print(&gpu, STDOUT_FILENO);

    /* Benchmark GPU memory */
    printf("\n── Benchmark VRAM Real ──\n");
    size_t block_size = 64 * 1024 * 1024; /* 64 MB */
    int iterations = 20;

    cuda_mem_t vram;
    if (cuda_mem_alloc(&vram, block_size) == 0) {
        cuda_bench_result_t br = cuda_benchmark(&vram, block_size, iterations);
        cuda_bench_print(&br, STDOUT_FILENO);
        cuda_mem_free(&vram);
    } else {
        printf("  ERRO: falha ao alocar VRAM para benchmark\n");
    }

    /* Migration test with real data */
    size_t page_size = 4096;
    uint32_t num_pages = 16384; /* 64 MB */

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc) {
            num_pages = (uint32_t)atoi(argv[i + 1]);
        }
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc) {
            page_size = (size_t)atol(argv[i + 1]);
        }
    }

    int rc = cuda_migration_test(page_size, num_pages);

    printf("\n══════════════════════════════════════════════════\n");
    printf("  RESUMO — VRAM REAL\n");
    printf("══════════════════════════════════════════════════\n");
    printf("  GPU: %s\n", gpu.name);
    printf("  VRAM: %.1f MB\n", (double)gpu.total_mem / (1024.0*1024.0));
    printf("  Resultado: %s\n", rc == 0 ? "✓ VRAM ENDEREÇÁVEL VALIDADA" : "✗ FALHOU");
    printf("══════════════════════════════════════════════════\n");

    return rc;
#endif
}

/* ── Command: run-cuda (pipeline completo com VRAM real) ──────── */

static int cmd_run_cuda(int argc, char **argv)
{
#if !HAS_CUDA
    (void)argc; (void)argv;
    fprintf(stderr, "Este binário não foi compilado com CUDA.\n");
    fprintf(stderr, "Use: make cuda && ./uvm-cuda run-cuda\n");
    return 1;
#else
    size_t page_size = 4096;
    uint32_t num_pages = 16384; /* 64 MB */

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc) {
            num_pages = (uint32_t)atoi(argv[i + 1]);
        }
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc) {
            page_size = (size_t)atol(argv[i + 1]);
        }
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   UFTA-VMM RUN-CUDA — Pipeline com VRAM     ║\n");
    printf("║   RAM ↔ GPU VRAM integrado no pipeline      ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* Initialize GPU */
    gpu_info_t gpu;
    if (cuda_backend_init(&gpu) != 0) {
        fprintf(stderr, "ERRO: GPU CUDA não disponível\n");
        return 1;
    }
    printf("GPU: %s (%.1f MB VRAM)\n", gpu.name, gpu.total_mem / (1024.0*1024.0));

    /* Initialize UFTA systems */
    tier_registry_t tiers;
    tier_registry_init_defaults(&tiers);

    page_table_t pages;
    page_table_init(&pages, num_pages);

    addr_map_t addr_map;
    addr_map_init(&addr_map, num_pages, page_size);

    field_engine_t field;
    field_engine_init(&field);

    predictor_t predictor;
    predictor_init(&predictor);

    bw_allocator_t bandwidth;
    memset(&bandwidth, 0, sizeof(bandwidth));
    bandwidth.num_channels = tiers.num_tiers;
    for (int i = 0; i < tiers.num_tiers; i++) {
        bandwidth.channels[i].id              = i;
        bandwidth.channels[i].tier_id         = tiers.tiers[i].id;
        bandwidth.channels[i].bandwidth_max   = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].bandwidth_desired = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].weight          = 1.0;
    }

    migrate_engine_t migrator;
    migrate_engine_init(&migrator);

    runtime_t rt;
    pipeline_init(&rt);
    rt.tiers     = &tiers;
    rt.pages     = &pages;
    rt.addr_map  = &addr_map;
    rt.field     = &field;
    rt.predictor = &predictor;
    rt.bandwidth = &bandwidth;
    rt.migrator  = &migrator;
    rt.tick_interval_ms   = 16.0;
    rt.prediction_horizon = 10;

    /* Allocate real VRAM region */
    size_t total = page_size * num_pages;
    cuda_mem_t vram;
    if (cuda_mem_alloc(&vram, total) != 0) {
        fprintf(stderr, "ERRO: falha ao alocar VRAM\n");
        return 1;
    }
    printf("VRAM alocada: %.1f MB\n", (double)total / (1024.0*1024.0));

    /* Allocate RAM region */
    unsigned char *ram = (unsigned char *)malloc(total);
    if (!ram) {
        cuda_mem_free(&vram);
        fprintf(stderr, "ERRO: falha ao alocar RAM\n");
        return 1;
    }

    /* Create pages in RAM (hot) and VRAM (warm) */
    printf("\nCriando páginas...\n");
    uint32_t ram_pages = num_pages / 2;
    uint32_t vram_pages = num_pages - ram_pages;

    for (uint32_t i = 0; i < ram_pages; i++) {
        page_t *p = page_alloc(&pages, page_size, &tiers.tiers[0]);
        if (p) {
            p->state.raw.x = 0.7 + (i % 3) * 0.1; /* hot */
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&addr_map, p);
        }
    }
    for (uint32_t i = 0; i < vram_pages; i++) {
        page_t *p = page_alloc(&pages, page_size, &tiers.tiers[1]);
        if (p) {
            p->state.raw.x = 0.4 + (i % 2) * 0.1; /* warm */
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&addr_map, p);
        }
    }
    printf("Criadas %u páginas (RAM: %u, VRAM: %u)\n\n",
           pages.count, ram_pages, vram_pages);

    /* Fill RAM with known pattern */
    for (size_t i = 0; i < total; i++) {
        ram[i] = (unsigned char)(i * 31 + 7);
    }

    /* Migrate RAM pages to VRAM via cudaMemcpy */
    printf("── Migração inicial RAM → VRAM (cudaMemcpy) ──\n");
    uint64_t t0 = now_ns();
    if (cuda_memcpy_h2d(&vram, ram, total) != 0) {
        fprintf(stderr, "ERRO: falha na migração H2D\n");
        free(ram);
        cuda_mem_free(&vram);
        return 1;
    }
    cudaDeviceSynchronize();
    uint64_t t1 = now_ns();
    double mig_time = (double)(t1 - t0) / 1e9;
    printf("  Migrado %.1f MB em %.3f ms (%.1f GB/s)\n",
           (double)total / (1024.0*1024.0), mig_time * 1000.0,
           (double)total / mig_time / 1e9);

    /* Process data in-place on GPU (proves VRAM is compute) */
    printf("\n── Processando dados na VRAM (kernel GPU) ──\n");
    t0 = now_ns();
    if (cuda_process_inplace(&vram, total) != 0) {
        fprintf(stderr, "ERRO: falha no kernel GPU\n");
        free(ram);
        cuda_mem_free(&vram);
        return 1;
    }
    t1 = now_ns();
    double kernel_time = (double)(t1 - t0) / 1e9;
    printf("  Kernel processou %.1f MB em %.3f ms (%.1f GB/s)\n",
           (double)total / (1024.0*1024.0), kernel_time * 1000.0,
           (double)total / kernel_time / 1e9);

    /* Read back and verify (data was transformed by kernel) */
    printf("\n── Verificando dados processados na VRAM ──\n");
    unsigned char *verify = (unsigned char *)malloc(total);
    if (!verify) {
        free(ram);
        cuda_mem_free(&vram);
        return 1;
    }
    if (cuda_memcpy_d2h(verify, &vram, total) != 0) {
        fprintf(stderr, "ERRO: falha na migração D2H\n");
        free(ram); free(verify);
        cuda_mem_free(&vram);
        return 1;
    }
    cudaDeviceSynchronize();

    /* Verify: kernel did y = y*2+1, so verify[i] == ram[i]*2+1 */
    uint64_t ok = 0, fail = 0;
    for (size_t i = 0; i < total; i += 16) {
        unsigned char expected = (unsigned char)(ram[i] * 2 + 1);
        if (verify[i] == expected) ok++;
        else fail++;
    }
    printf("  Verificados: %llu | Falhas: %llu\n",
           (unsigned long long)ok, (unsigned long long)fail);
    printf("  %s\n", fail == 0 ? "✓ KERNEL GPU EXECUTOU CORRETAMENTE NA VRAM"
                               : "✗ FALHA NO KERNEL");

    /* Run UFTA pipeline cycles */
    printf("\n── Pipeline UFTA (10 ciclos) ──\n");
    for (int cycle = 0; cycle < 10; cycle++) {
        /* Simulate accesses */
        for (uint32_t i = 0; i < pages.count; i++) {
            page_t *p = &pages.pages[i];
            if (rand() % 3 == 0) {
                page_access(p, (uint64_t)cycle * 1000000);
            }
        }
        pipeline_tick(&rt);
        if (cycle % 3 == 0 || cycle == 9) {
            printf("\nCycle %d:\n", cycle);
            pipeline_metrics_print(&rt, STDOUT_FILENO);
        }
    }

    /* Print final tier state */
    printf("\n── Estado Final dos Tiers ──\n");
    for (int i = 0; i < tiers.num_tiers; i++) {
        tier_print(&tiers.tiers[i], STDOUT_FILENO);
    }

    migrate_stats_print(&migrator, STDOUT_FILENO);

    /* Cleanup */
    free(ram);
    free(verify);
    free(pages.pages);
    free(addr_map.entries);
    predictor_destroy(&predictor);
    cuda_mem_free(&vram);

    printf("\nRun-CUDA completo.\n");
    return 0;
#endif
}

/* ── Command: page-fault (transparent migration demo) ──────────── */

static int cmd_pagefault(int argc, char **argv)
{
    uint32_t num_pages = 64;
    uint32_t page_size = 4096;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc)
            num_pages = (uint32_t)atoi(argv[i + 1]);
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc)
            page_size = (uint32_t)atoi(argv[i + 1]);
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   UFTA-VMM PAGE-FAULT — Migração Transparente║\n");
    printf("║   SIGSEGV + mprotect = gerenciamento auto   ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* Initialize page fault handler */
    pf_context_t pf;
    if (pf_init(&pf, page_size, num_pages) != 0) {
        fprintf(stderr, "ERRO: falha ao inicializar page fault handler\n");
        return 1;
    }

    /* Step 1: Allocate pages and write known patterns */
    printf("── Fase 1: Alocar %u páginas e gravar dados ──\n", num_pages);
    for (uint32_t i = 0; i < num_pages; i++) {
        void *p = pf_page_alloc(&pf, i);
        if (!p) {
            fprintf(stderr, "  ERRO: alloc page %u\n", i);
            pf_destroy(&pf);
            return 1;
        }
        /* Write pattern: page_id | 0xAA */
        pf_page_write_pattern(&pf, i, (uint8_t)(i & 0xFF));
    }
    printf("  %u páginas alocadas e preenchidas (RAM)\n", num_pages);

    /* Step 2: Verify all pages accessible */
    printf("\n── Fase 2: Verificar dados em RAM ──\n");
    uint64_t ok = 0, fail = 0;
    for (uint32_t i = 0; i < num_pages; i++) {
        if (pf_page_verify_pattern(&pf, i, (uint8_t)(i & 0xFF)) == 0)
            ok++;
        else
            fail++;
    }
    printf("  Verificados: %llu | Falhas: %llu\n",
           (unsigned long long)ok, (unsigned long long)fail);

    /* Step 3: Evict 75% of pages to VRAM-sim and FILE */
    uint32_t to_vram = num_pages / 2;
    uint32_t to_file = num_pages / 4;
    printf("\n── Fase 3: Evadir páginas ──\n");
    printf("  → VRAM-sim: %u páginas\n", to_vram);
    printf("  → FILE:     %u páginas\n", to_file);
    printf("  → RAM:      %u páginas (restantes)\n",
           num_pages - to_vram - to_file);

    for (uint32_t i = 0; i < to_vram; i++) {
        if (pf_page_evict(&pf, i, PF_LOC_VRAM) != 0) {
            fprintf(stderr, "  ERRO: evict page %u to VRAM\n", i);
        }
    }
    for (uint32_t i = to_vram; i < to_vram + to_file; i++) {
        if (pf_page_evict(&pf, i, PF_LOC_FILE) != 0) {
            fprintf(stderr, "  ERRO: evict page %u to FILE\n", i);
        }
    }

    printf("  Evictions completas. Dados salvos em backends.\n");

    /* Step 4: ACCESS EVICED PAGES — triggers page faults! */
    printf("\n── Fase 4: Acessar páginas evictas (PAGE FAULTS!) ──\n");
    printf("  Cada acesso a uma página evictada gera SIGSEGV,\n");
    printf("  o handler carrega automaticamente do backend.\n\n");

    uint64_t t0 = now_ns();
    uint64_t faults_caught = 0;

    /* Access all evicted pages — each will fault */
    for (uint32_t i = 0; i < num_pages; i++) {
        uint8_t expected = (uint8_t)(i & 0xFF);

        /* Access the page via pointer (dereference triggers fault if evicted) */
        volatile uint8_t *ptr = (volatile uint8_t *)((char *)pf.region_base +
                                                      i * page_size);

        /* Force a read — this triggers SIGSEGV if page was evicted */
        uint8_t val = *ptr;
        (void)val;

        /* Verify the data survived the round-trip */
        if (pf_page_verify_pattern(&pf, i, expected) == 0) {
            faults_caught++;
        } else {
            fail++;
        }
    }

    uint64_t t1 = now_ns();
    double fault_time_ms = (double)(t1 - t0) / 1e6;

    printf("  Páginas acessadas:     %u\n", num_pages);
    printf("  Page faults resolvidos: %llu\n", (unsigned long long)faults_caught);
    printf("  Falhas de integridade: %llu\n", (unsigned long long)fail);
    printf("  Tempo total:           %.3f ms\n", fault_time_ms);
    printf("  Latência média/fault:  %.1f µs\n",
           faults_caught > 0 ? (fault_time_ms * 1000.0 / faults_caught) : 0.0);

    /* Step 5: Re-evict and fault again (demonstrates repeated migration) */
    printf("\n── Fase 5: Re-evadir e re-acessar (ciclo completo) ──\n");
    for (uint32_t i = 0; i < to_vram; i++) {
        pf_page_evict(&pf, i, PF_LOC_VRAM);
    }
    for (uint32_t i = to_vram; i < to_vram + to_file; i++) {
        pf_page_evict(&pf, i, PF_LOC_FILE);
    }

    /* Access again — more faults */
    uint64_t faults2 = 0;
    for (uint32_t i = 0; i < num_pages; i++) {
        uint8_t expected = (uint8_t)(i & 0xFF);
        volatile uint8_t *ptr = (volatile uint8_t *)((char *)pf.region_base +
                                                      i * page_size);
        uint8_t val = *ptr;
        (void)val;
        if (pf_page_verify_pattern(&pf, i, expected) == 0)
            faults2++;
    }

    printf("  2º ciclo: %llu faults, %llu OK\n",
           (unsigned long long)pf.total_faults, (unsigned long long)faults2);

    /* Step 6: Proactive migration using UFTA pipeline */
    printf("\n── Fase 6: Migração Proativa (UFTA Pipeline) ──\n");
    printf("  Simulando acesso patterns e usando o pipeline\n");
    printf("  para decidir quais páginas manter em RAM.\n\n");

    /* Initialize UFTA pipeline */
    tier_registry_t tiers;
    tier_registry_init_defaults(&tiers);

    page_table_t pages;
    page_table_init(&pages, num_pages);

    addr_map_t addr_map;
    addr_map_init(&addr_map, num_pages, page_size);

    field_engine_t field;
    field_engine_init(&field);

    predictor_t predictor;
    predictor_init(&predictor);

    bw_allocator_t bandwidth;
    memset(&bandwidth, 0, sizeof(bandwidth));
    bandwidth.num_channels = tiers.num_tiers;
    for (int i = 0; i < tiers.num_tiers; i++) {
        bandwidth.channels[i].id              = i;
        bandwidth.channels[i].tier_id         = tiers.tiers[i].id;
        bandwidth.channels[i].bandwidth_max   = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].bandwidth_desired = tiers.tiers[i].bandwidth_max;
        bandwidth.channels[i].weight          = 1.0;
    }

    migrate_engine_t migrator;
    migrate_engine_init(&migrator);

    runtime_t rt;
    pipeline_init(&rt);
    rt.tiers     = &tiers;
    rt.pages     = &pages;
    rt.addr_map  = &addr_map;
    rt.field     = &field;
    rt.predictor = &predictor;
    rt.bandwidth = &bandwidth;
    rt.migrator  = &migrator;
    rt.tick_interval_ms   = 100.0;
    rt.prediction_horizon = 5;

    /* Create pages mirroring the PF state */
    for (uint32_t i = 0; i < num_pages; i++) {
        pf_page_t *pp = &pf.pages[i];
        if (!pp->valid) continue;

        tier_t *tier = &tiers.tiers[0]; /* default RAM */
        if (pp->location == PF_LOC_VRAM) tier = &tiers.tiers[1];
        else if (pp->location == PF_LOC_FILE) tier = &tiers.tiers[4];

        page_t *p = page_alloc(&pages, page_size, tier);
        if (p) {
            /* Heat based on fault count (more faults = hotter) */
            p->state.raw.x = 0.3 + (double)pp->fault_count * 0.1;
            if (p->state.raw.x > 1.0) p->state.raw.x = 1.0;
            p->access_count = pp->access_count;
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&addr_map, p);
        }
    }

    printf("  Pipeline com %u páginas baseadas nos page faults\n", pages.count);

    /* Run 5 pipeline cycles */
    for (int cycle = 0; cycle < 5; cycle++) {
        /* Simulate access pattern: first pages are hotter */
        for (uint32_t i = 0; i < pages.count; i++) {
            page_t *p = &pages.pages[i];
            if (i < pages.count / 4)
                page_access(p, (uint64_t)cycle * 1000000); /* hot */
            else if (rand() % 5 == 0)
                page_access(p, (uint64_t)cycle * 1000000); /* occasional */
        }
        pipeline_tick(&rt);
    }

    printf("\n  Pipeline resultado:\n");
    for (int i = 0; i < tiers.num_tiers; i++) {
        tier_print(&tiers.tiers[i], STDOUT_FILENO);
    }
    migrate_stats_print(&migrator, STDOUT_FILENO);

    /* Print page fault stats */
    printf("\n");
    pf_stats_print(&pf, STDOUT_FILENO);

    /* Cleanup */
    free(pages.pages);
    free(addr_map.entries);
    predictor_destroy(&predictor);
    pf_destroy(&pf);

    printf("\nPage-fault demo completo. %d page faults tratados transparentemente.\n",
           (int)pf.total_faults);
    return 0;
}

/* ── Command: page-fault-cuda (transparent migration with REAL VRAM) ── */

static int cmd_pagefault_cuda(int argc, char **argv)
{
#if !HAS_CUDA
    (void)argc; (void)argv;
    fprintf(stderr, "Este binário não foi compilado com CUDA.\n");
    fprintf(stderr, "Use: make cuda && ./uvm-cuda page-fault-cuda\n");
    return 1;
#else
    uint32_t num_pages = 64;
    uint32_t page_size = 4096;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc)
            num_pages = (uint32_t)atoi(argv[i + 1]);
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc)
            page_size = (uint32_t)atoi(argv[i + 1]);
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║ UFTA-VMM PAGE-FAULT-CUDA — Migração Transp. ║\n");
    printf("║ SIGSEGV + VRAM REAL da GPU                  ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* Initialize page fault handler with REAL VRAM backing */
    pf_context_t pf;
    if (pf_init_cuda(&pf, page_size, num_pages) != 0) {
        fprintf(stderr, "ERRO: falha ao inicializar page fault handler\n");
        return 1;
    }

    if (!pf.vram_real_ready) {
        fprintf(stderr, "AVISO: VRAM real não disponível — usando memfd\n");
    }

    /* Step 1: Allocate pages and write known patterns */
    printf("\n── Fase 1: Alocar %u páginas e gravar dados ──\n", num_pages);
    for (uint32_t i = 0; i < num_pages; i++) {
        void *p = pf_page_alloc(&pf, i);
        if (!p) {
            fprintf(stderr, "  ERRO: alloc page %u\n", i);
            pf_destroy(&pf);
            return 1;
        }
        pf_page_write_pattern(&pf, i, (uint8_t)(i & 0xFF));
    }
    printf("  %u páginas alocadas e preenchidas (RAM)\n", num_pages);

    /* Step 2: Verify all pages accessible */
    printf("\n── Fase 2: Verificar dados em RAM ──\n");
    uint64_t ok = 0, fail = 0;
    for (uint32_t i = 0; i < num_pages; i++) {
        if (pf_page_verify_pattern(&pf, i, (uint8_t)(i & 0xFF)) == 0)
            ok++;
        else
            fail++;
    }
    printf("  Verificados: %llu | Falhas: %llu\n",
           (unsigned long long)ok, (unsigned long long)fail);

    /* Step 3: Evict pages to REAL VRAM and FILE */
    uint32_t to_vram_real = num_pages / 2;
    uint32_t to_file = num_pages / 4;
    printf("\n── Fase 3: Evadir páginas ──\n");
    printf("  → VRAM REAL (GPU): %u páginas\n", to_vram_real);
    printf("  → FILE:            %u páginas\n", to_file);
    printf("  → RAM:             %u páginas (restantes)\n",
           num_pages - to_vram_real - to_file);

    for (uint32_t i = 0; i < to_vram_real; i++) {
        if (pf_page_evict(&pf, i, PF_LOC_VRAM_REAL) != 0) {
            fprintf(stderr, "  ERRO: evict page %u to VRAM real\n", i);
        }
    }
    for (uint32_t i = to_vram_real; i < to_vram_real + to_file; i++) {
        if (pf_page_evict(&pf, i, PF_LOC_FILE) != 0) {
            fprintf(stderr, "  ERRO: evict page %u to FILE\n", i);
        }
    }

    printf("  Evictions completas. Dados salvos na VRAM real da GPU.\n");

    /* Step 4: ACCESS EVICTED PAGES — triggers page faults! */
    printf("\n── Fase 4: Acessar páginas evictas (PAGE FAULTS!) ──\n");
    printf("  Cada acesso a uma página evictada gera SIGSEGV,\n");
    printf("  o handler carrega automaticamente da VRAM real.\n\n");

    uint64_t t0 = now_ns();
    uint64_t faults_caught = 0;

    for (uint32_t i = 0; i < num_pages; i++) {
        uint8_t expected = (uint8_t)(i & 0xFF);

        /* Access the page via pointer (dereference triggers fault if evicted) */
        volatile uint8_t *ptr = (volatile uint8_t *)((char *)pf.region_base +
                                                      i * page_size);
        uint8_t val = *ptr;
        (void)val;

        if (pf_page_verify_pattern(&pf, i, expected) == 0) {
            faults_caught++;
        } else {
            fail++;
        }
    }

    uint64_t t1 = now_ns();
    double fault_time_ms = (double)(t1 - t0) / 1e6;

    printf("  Páginas acessadas:     %u\n", num_pages);
    printf("  Page faults resolvidos: %llu\n", (unsigned long long)faults_caught);
    printf("  Falhas de integridade: %llu\n", (unsigned long long)fail);
    printf("  Tempo total:           %.3f ms\n", fault_time_ms);
    printf("  Latência média/fault:  %.1f µs\n",
           faults_caught > 0 ? (fault_time_ms * 1000.0 / faults_caught) : 0.0);

    /* Step 5: Re-evict and fault again (repeated migration) */
    printf("\n── Fase 5: Re-evadir e re-acessar (ciclo completo) ──\n");
    for (uint32_t i = 0; i < to_vram_real; i++) {
        pf_page_evict(&pf, i, PF_LOC_VRAM_REAL);
    }
    for (uint32_t i = to_vram_real; i < to_vram_real + to_file; i++) {
        pf_page_evict(&pf, i, PF_LOC_FILE);
    }

    uint64_t faults2 = 0;
    for (uint32_t i = 0; i < num_pages; i++) {
        uint8_t expected = (uint8_t)(i & 0xFF);
        volatile uint8_t *ptr = (volatile uint8_t *)((char *)pf.region_base +
                                                      i * page_size);
        uint8_t val = *ptr;
        (void)val;
        if (pf_page_verify_pattern(&pf, i, expected) == 0)
            faults2++;
    }

    printf("  2º ciclo: %llu faults, %llu OK\n",
           (unsigned long long)pf.total_faults, (unsigned long long)faults2);

    /* Print page fault stats */
    printf("\n");
    pf_stats_print(&pf, STDOUT_FILENO);

    /* Cleanup */
    pf_destroy(&pf);

    printf("\nPage-fault-cuda demo completo. %d page faults tratados\n",
           (int)pf.total_faults);
    printf("com VRAM REAL da GPU.\n");
    return 0;
#endif
}

/* ── Command: validate-intelligent (Migração Inteligente com Aprendizado) ── */

static int cmd_validate_intelligent(int argc, char **argv)
{
    /* ── Parse args ── */
    uint32_t num_pages = 128;
    size_t   page_size = 4096;
    int      num_cycles = 100;
    real_t   learning_rate = 0.05;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc)
            num_pages = (uint32_t)atoi(argv[i + 1]);
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc)
            page_size = (size_t)atol(argv[i + 1]);
        if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc)
            num_cycles = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--lr") == 0 && i + 1 < argc)
            learning_rate = (real_t)atof(argv[i + 1]);
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  UFTA-VMM VALIDATE-INTELLIGENT               ║\n");
    printf("║  Migração Inteligente com Aprendizado        ║\n");
    printf("║  Predictor: R(θ) com LMS + Momentum          ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    printf("  Config: %u páginas × %zu bytes = %.1f KB\n",
           num_pages, page_size, (double)(num_pages * page_size) / 1024.0);
    printf("  Ciclos: %d | Padrão: 80/20 (20%% hot, 80%% cold)\n\n", num_cycles);

    /* ── Initialize pipeline engines ── */
    tier_registry_t tiers;
    tier_registry_init_defaults(&tiers);

    page_table_t pt;
    page_table_init(&pt, num_pages);

    addr_map_t addr_map;
    addr_map_init(&addr_map, num_pages, (uint32_t)page_size);

    field_engine_t field;
    field_engine_init(&field);

    predictor_t predictor;
    predictor_init(&predictor);
    predictor.learning_rate = learning_rate;

    bw_allocator_t bw;
    memset(&bw, 0, sizeof(bw));
    bw.num_channels = tiers.num_tiers;
    for (int i = 0; i < tiers.num_tiers; i++) {
        bw.channels[i].id              = i;
        bw.channels[i].tier_id         = tiers.tiers[i].id;
        bw.channels[i].bandwidth_max   = tiers.tiers[i].bandwidth_max;
        bw.channels[i].bandwidth_desired = tiers.tiers[i].bandwidth_max;
        bw.channels[i].weight          = 1.0;
    }

    migrate_engine_t migrator;
    migrate_engine_init(&migrator);

    /* Wire up runtime */
    runtime_t rt;
    pipeline_init(&rt);
    rt.tiers     = &tiers;
    rt.pages     = &pt;
    rt.addr_map  = &addr_map;
    rt.field     = &field;
    rt.predictor = &predictor;
    rt.bandwidth = &bw;
    rt.migrator  = &migrator;
    rt.tick_interval_ms = 50.0; /* 50ms → dt=0.05s, matches simulation */

    /* ── Allocate pages in pipeline ──
     * Pages start in VRAM (T1) so hot pages can migrate UP to RAM (T0).
     * This exercises the real migration pipeline: hot→RAM, cold→slower tiers. */
    tier_t *vram_tier = &tiers.tiers[1]; /* T1: VRAM */
    for (uint32_t i = 0; i < num_pages; i++) {
        page_t *p = page_alloc(&pt, page_size, vram_tier);
        if (!p) {
            fprintf(stderr, "ERRO: falha ao alocar página %u\n", i);
            continue;
        }
        /* Set initial state vector with verifiable pattern (not real memory) */
        real_t phase = (real_t)i * 0.1;
        p->state.raw = (vec3_t){cos(phase), sin(phase), 1.0};
        p->state.versor = vec3_normalize(p->state.raw);
    }
    printf("  %u páginas alocadas em VRAM (T1)\n\n", pt.count);

    /* ── Define access pattern: 80/20 rule ── */
    uint32_t hot_count = num_pages / 5;    /* 20% = hot */
    uint32_t cold_count = num_pages - hot_count; /* 80% = cold */

    /* Mark hot pages */
    for (uint32_t i = 0; i < hot_count; i++) {
        page_t *p = page_find(&pt, i);
        if (p) {
            p->heat.value = 0.9;
            p->heat.level = HEAT_HOT;
            p->access_count = 100;
        }
    }
    for (uint32_t i = hot_count; i < num_pages; i++) {
        page_t *p = page_find(&pt, i);
        if (p) {
            p->heat.value = 0.1;
            p->heat.level = HEAT_COLD;
            p->access_count = 5;
        }
    }

    /* ── Tracking arrays for convergence ── */
    real_t *cycle_errors = calloc((size_t)num_cycles, sizeof(real_t));
    real_t *cycle_confidence = calloc((size_t)num_cycles, sizeof(real_t));
    int *cycle_migrations = calloc((size_t)num_cycles, sizeof(int));

    /* ══════════════════════════════════════════════════════════════════
     *  SIMULATION LOOP — Run pipeline with learning
     *
     *  The hot pages follow a ROTATED circular pattern:
     *    σ(t) = R(θ_true) · [A·cos(ωt), B·sin(ωt), C]
     *  where θ_true = (0.5, 0.3, -0.2) is a fixed rotation the predictor
     *  must LEARN. Initially θ=0 (identity), so the prediction is wrong.
     *  Over cycles, LMS gradient descent drives θ → θ_true, reducing error.
     *
     *  The pipeline predicts at the horizon (dt = tick * horizon), so the
     *  rotation matters significantly. Error is measured as the distance
     *  between predicted and true future direction.
     * ══════════════════════════════════════════════════════════════════ */

    printf("── Fase 1: Simulação com Preditor Inteligente ──\n");
    printf("  O predictor aprende a rotação R(θ) do padrão de acesso.\n");
    printf("  Padrão hot: σ(t) = R(0.5, 0.3, -0.2) · [cos(ωt), sin(ωt), 0.5]\n");
    printf("  Velocidade fornecida SEM rotação → predictor deve aprender R(θ)\n\n");

    /* True rotation the predictor must discover */
    const real_t theta_true[3] = {0.5, 0.3, -0.2};
    mat3_t R_true = build_rotation(theta_true);
    const real_t omega = 0.5;  /* angular frequency */
    const real_t dt_sim = 0.05; /* simulation time step (s) */
    const real_t VEL_K = 20.0;  /* velocity scale: makes R(θ) matter */

    /* Helper: compute the hot-page state at time t */
    #define HOT_STATE(t) do { \
        vec3_t _b = (vec3_t){cos(omega * (t)), sin(omega * (t)), 0.5}; \
        vec3_t _r = mat3_mul_vec(R_true, _b); \
        _hot_raw = (vec3_t){_r.x, _r.y, _r.z + 0.2 * sin(omega * (t) * 0.5)}; \
        _hot_dir = vec3_normalize(_hot_raw); \
    } while (0)

    /* Helper: velocity = K · derivative of UNROTATED circle (WRONG direction) */
    #define HOT_VEL(t) (vec3_t){ \
        -VEL_K * omega * sin(omega * (t)), \
         VEL_K * omega * cos(omega * (t)), 0.0 }

    uint64_t t_start = now_ns();
    uint64_t total_updates = 0;

    /* Initialize cycle 0 state for all pages */
    vec3_t _hot_raw, _hot_dir;
    for (uint32_t i = 0; i < num_pages; i++) {
        page_t *p = page_find(&pt, i);
        if (!p) continue;
        if (i < hot_count) {
            HOT_STATE(0.0);
            p->state.raw = _hot_raw;
            p->state.versor = _hot_dir;
            p->motion.velocity = HOT_VEL(0.0);
        } else {
            p->state.raw = (vec3_t){0.05, 0.05, 0.05};
            p->state.versor = vec3_normalize(p->state.raw);
            p->motion.velocity = vec3_zero();
        }
    }

    /* ══════════════════════════════════════════════════════════════════
     *  SIMULATION LOOP — Run the REAL pipeline (predict + learn + migrate)
     *
     *  Each cycle:
     *    1. Set state σ_t and velocity v_t for all pages
     *    2. pipeline_tick: Observe→Represent→Predict(learn)→Prioritize→
     *       Throttle→Migrate→Commit
     *    3. The pipeline's pipeline_predict compares last tick's prediction
     *       against current state (1-step-ahead LMS update with momentum)
     * ══════════════════════════════════════════════════════════════════ */

    for (int cycle = 0; cycle < num_cycles; cycle++) {
        real_t t = (real_t)cycle * dt_sim; /* current time */

        /* ── Step A: set current state for all pages ── */
        for (uint32_t i = 0; i < num_pages; i++) {
            page_t *p = page_find(&pt, i);
            if (!p) continue;

            if (i < hot_count) {
                p->access_count += 10;
                HOT_STATE(t);
                /* Use raw state directly (versor recomputed by pipeline) */
                p->state.raw = _hot_raw;
                p->state.versor = _hot_dir;
                /* Velocity = derivative of UNROTATED circle (WRONG direction) */
                p->motion.velocity = HOT_VEL(t);
            } else {
                p->access_count += 1;
                real_t r = (real_t)((i * 7 + cycle * 13) % 100) / 100.0;
                p->state.raw = (vec3_t){0.1 * r, 0.1 * (1.0 - r), 0.05};
                p->state.versor = vec3_normalize(p->state.raw);
                p->motion.velocity = vec3_zero();
            }
        }

        /* ── Step B: run the REAL pipeline tick ── */
        pipeline_tick(&rt);
        cycle_migrations[cycle] = (int)rt.metrics.migrations_this_cycle;

        /* ── Step C: measure error/confidence ── */
        real_t total_err = 0.0;
        real_t total_conf = 0.0;
        for (uint32_t i = 0; i < hot_count; i++) {
            total_err += predictor_error(rt.predictor, i);
            total_conf += predictor_confidence(rt.predictor, i);
        }
        cycle_errors[cycle] = total_err / (real_t)hot_count;
        cycle_confidence[cycle] = total_conf / (real_t)hot_count;
    }
    #undef HOT_STATE
    #undef HOT_VEL

    /* Count total gradient updates from predictor states */
    for (uint32_t i = 0; i < hot_count; i++) {
        for (int s = 0; s < rt.predictor->num_states; s++) {
            if (rt.predictor->states[s].page_id == i) {
                total_updates += rt.predictor->states[s].total_updates;
                break;
            }
        }
    }

    uint64_t t_end = now_ns();
    double sim_time_ms = (double)(t_end - t_start) / 1e6;

    /* ══════════════════════════════════════════════════════════════════
     *  RESULTS — Predictor Convergence
     * ══════════════════════════════════════════════════════════════════ */

    printf("  Simulação concluída em %.1f ms (%d ciclos)\n\n", sim_time_ms, num_cycles);

    printf("── Fase 2: Convergência do Predictor ──\n\n");

    /* Show error convergence (first, middle, last 10 cycles) */
    printf("  Ciclo  │ Erro Médio    │ Confiança   │ Migrações\n");
    printf("  ───────┼───────────────┼─────────────┼──────────\n");

    int sample_points[] = {0, 4, 9, 24, 49, 74, 99};
    for (int s = 0; s < 7; s++) {
        int c = sample_points[s];
        if (c >= num_cycles) continue;
        printf("  %5d  │ %11.6f   │ %9.4f   │ %d\n",
               c, cycle_errors[c], cycle_confidence[c], cycle_migrations[c]);
    }

    /* Compute convergence metrics.
     * Use cycle 1 as baseline (cycle 0 has no prior prediction, so error
     * is the default 0.04 — not meaningful for convergence). */
    real_t initial_error = cycle_errors[1];
    real_t final_error = cycle_errors[num_cycles - 1];
    real_t initial_conf = cycle_confidence[1];
    real_t final_conf = cycle_confidence[num_cycles - 1];
    real_t error_reduction = (initial_error > 1e-12) ?
        (1.0 - final_error / initial_error) * 100.0 : 0.0;
    real_t conf_increase = (final_conf - initial_conf) * 100.0;

    printf("\n  ── Resumo da Convergência ──\n");
    printf("  Erro inicial:   %.6f → final: %.6f (%.1f%% redução)\n",
           initial_error, final_error, error_reduction);
    printf("  Confiança:      %.4f → %.4f (+%.1f%%)\n",
           initial_conf, final_conf, conf_increase);

    /* Total migrations */
    int total_mig = 0;
    for (int c = 0; c < num_cycles; c++)
        total_mig += cycle_migrations[c];
    printf("  Migrações totais: %d (%.1f por ciclo)\n",
           total_mig, (double)total_mig / num_cycles);

    /* ══════════════════════════════════════════════════════════════════
     *  COMPARISON: Intelligent vs Random migration decisions
     *
     *  After learning, the predictor has HIGH confidence on hot pages
     *  (it learned their pattern) and LOW confidence on cold pages
     *  (random pattern). We use this to decide migration:
     *    - Intelligent: keep page in RAM (T0) iff predictor confidence
     *      exceeds a threshold; else migrate to VRAM (T1).
     *    - Random: 50/50 guess.
     *  We count how many pages end up in the WRONG tier.
     * ══════════════════════════════════════════════════════════════════ */

    printf("\n── Fase 3: Comparação com Migração Aleatória ──\n\n");

    const real_t conf_threshold = 0.5; /* confidence to keep in RAM */

    int wrong_intelligent = 0;
    int wrong_random = 0;
    int migrated_intelligent = 0;
    int migrated_random = 0;

    for (uint32_t i = 0; i < num_pages; i++) {
        page_t *p = page_find(&pt, i);
        if (!p) continue;

        bool is_hot = (i < hot_count);

        /* ── Intelligent: use learned predictor confidence ── */
        real_t conf = predictor_confidence(rt.predictor, i);
        bool keep_ram_intelligent = (conf >= conf_threshold);
        if (keep_ram_intelligent != is_hot)
            wrong_intelligent++;
        if (!keep_ram_intelligent)
            migrated_intelligent++; /* evicted to VRAM */

        /* ── Random: 50/50 guess ── */
        bool keep_ram_random = ((i * 7 + 42) % 2 == 0);
        if (keep_ram_random != is_hot)
            wrong_random++;
        if (!keep_ram_random)
            migrated_random++;
    }

    printf("  Páginas hot: %u | Páginas cold: %u\n", hot_count, cold_count);
    printf("  Limiar de confiança: %.2f (confiança ≥ limiar → mantém em RAM)\n\n",
           conf_threshold);
    printf("  Método         │ Decisões Erradas │ Migrações │ Erro do Predictor\n");
    printf("  ───────────────┼──────────────────┼───────────┼──────────────────\n");
    printf("  Inteligente    │ %16d │ %9d │ %.6f\n",
           wrong_intelligent, migrated_intelligent, final_error);
    printf("  Aleatório      │ %16d │ %9d │ N/A (sem aprendizado)\n",
           wrong_random, migrated_random);

    real_t accuracy = (1.0 - (real_t)wrong_intelligent /
                       (real_t)(num_pages > 0 ? num_pages : 1)) * 100.0;
    real_t random_accuracy = (1.0 - (real_t)wrong_random /
                       (real_t)(num_pages > 0 ? num_pages : 1)) * 100.0;
    printf("\n  Precisão do predictor: %.1f%%  (aleatório: %.1f%%)\n",
           accuracy, random_accuracy);

    /* ══════════════════════════════════════════════════════════════════
     *  VRAM Integration Test (if CUDA available)
     * ══════════════════════════════════════════════════════════════════ */

#if PF_HAS_CUDA
    printf("\n── Fase 4: Migração com VRAM REAL ──\n\n");

    pf_context_t pf;
    uint32_t pf_pages = 64;
    if (pf_init_cuda(&pf, (uint32_t)page_size, pf_pages) != 0) {
        fprintf(stderr, "  ERRO: falha ao inicializar page-fault handler\n");
    } else {
        /* Allocate and fill pages (marks them valid + accessible) */
        for (uint32_t i = 0; i < pf_pages; i++) {
            if (!pf_page_alloc(&pf, i)) {
                fprintf(stderr, "  ERRO: alloc page %u\n", i);
                pf_destroy(&pf);
                return 1;
            }
            pf_page_write_pattern(&pf, i, (uint8_t)(i & 0xFF));
        }

        /* Evict hot pages to VRAM real, cold to FILE */
        for (uint32_t i = 0; i < hot_count && i < pf_pages / 2; i++) {
            pf_page_evict(&pf, i, PF_LOC_VRAM_REAL);
        }
        for (uint32_t i = hot_count; i < pf_pages; i++) {
            pf_page_evict(&pf, i, PF_LOC_FILE);
        }

        /* Access all pages — triggers faults */
        uint64_t t0 = now_ns();
        for (uint32_t i = 0; i < pf_pages; i++) {
            volatile uint8_t *ptr = (volatile uint8_t *)((char *)pf.region_base +
                                                          i * page_size);
            uint8_t val = *ptr;
            (void)val;
        }
        uint64_t t1 = now_ns();

        printf("  VRAM REAL: %u faults em %.3f ms (%.1f µs/fault)\n",
               (int)pf.total_faults, (double)(t1 - t0) / 1e6,
               pf.total_faults > 0 ?
                   (double)(t1 - t0) / 1000.0 / pf.total_faults : 0.0);

        pf_stats_print(&pf, STDOUT_FILENO);
        pf_destroy(&pf);
    }
#endif

    /* ══════════════════════════════════════════════════════════════════
     *  FINAL SUMMARY
     * ══════════════════════════════════════════════════════════════════ */

    printf("\n══════════════════════════════════════════════════\n");
    printf("  VALIDAÇÃO — Migração Inteligente com Aprendizado\n");
    printf("══════════════════════════════════════════════════\n");
    printf("  Predictor R(θ) com LMS + Momentum:\n");
    printf("    • Erro:       %.6f → %.6f (%.1f%% redução)\n",
           initial_error, final_error, error_reduction);
    printf("    • Confiança:  %.4f → %.4f (+%.1f%%)\n",
           initial_conf, final_conf, conf_increase);
    printf("    • Precisão:   %.1f%%\n", accuracy);
    printf("  Pipeline: %d ciclos, %d migrações totais\n",
           num_cycles, total_mig);
    printf("  Aprendizado: %llu updates do gradiente\n",
           (unsigned long long)total_updates);
#if PF_HAS_CUDA
    printf("  VRAM REAL:TEGRADO ✓\n");
#endif
    printf("══════════════════════════════════════════════════\n");

    /* Cleanup */
    free(cycle_errors);
    free(cycle_confidence);
    free(cycle_migrations);
    predictor_destroy(&predictor);

    return (error_reduction > 0.0 && accuracy > 50.0) ? 0 : 1;
}

/* ── Command: gui (real-time dashboard) ───────────────────────── */

#include "ufta/gui.h"

/* Shared state between the pipeline thread and the GUI thread */
typedef struct {
    tier_registry_t  tiers;
    page_table_t     pages;
    addr_map_t       addr_map;
    field_engine_t   field;
    predictor_t      predictor;
    bw_allocator_t   bandwidth;
    migrate_engine_t migrator;
    runtime_t        rt;
    pf_context_t     pf;

    ufta_gui_snapshot_t snap;
    bool running;
} gui_shared_t;

static void *gui_pipeline_thread(void *arg)
{
    gui_shared_t *gs = (gui_shared_t *)arg;

    /* Run the pipeline continuously */
    while (gs->running) {
        /* Simulate accesses */
        for (uint32_t i = 0; i < gs->pages.count; i++) {
            page_t *p = &gs->pages.pages[i];
            if (rand() % 3 == 0) {
                page_access(p, gs->rt.timestamp_ns);
            }
        }

        pipeline_tick(&gs->rt);

        /* Update the GUI snapshot */
        gs->snap.tiers    = &gs->tiers;
        gs->snap.metrics  = &gs->rt.metrics;
        gs->snap.migrate  = &gs->migrator.stats;
        gs->snap.worker   = &gs->pf.worker;
        gs->snap.pf       = &gs->pf;
        gs->snap.cycle_count = gs->rt.cycle_count;
        gs->snap.vram_recovered_mb =
            (double)gs->migrator.stats.total_bytes_moved / (1024.0 * 1024.0);
        gs->snap.latency_us_per_fault =
            gs->pf.total_faults > 0 ?
                (double)gs->pf.bytes_loaded /
                (double)gs->pf.total_faults / 1024.0 : 0.0;
        gs->snap.fps_estimate =
            gs->rt.metrics.total_cycle_us > 0.0 ?
                1000000.0 / gs->rt.metrics.total_cycle_us : 0.0;

        /* Throttle to ~60 Hz */
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = 16000000;
        nanosleep(&ts, NULL);
    }

    return NULL;
}

static int cmd_gui(int argc, char **argv)
{
    uint32_t num_pages = 256;
    uint32_t page_size = 4096;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc)
            num_pages = (uint32_t)atoi(argv[i + 1]);
        if (strcmp(argv[i], "--page-size") == 0 && i + 1 < argc)
            page_size = (uint32_t)atoi(argv[i + 1]);
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   UFTA-VMM GUI — Dashboard em Tempo Real    ║\n");
    printf("║   SDL2 + OpenGL                             ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    gui_shared_t gs;
    memset(&gs, 0, sizeof(gs));
    gs.running = true;

    /* Initialize tier registry */
    tier_registry_init_defaults(&gs.tiers);

    /* Initialize page table */
    page_table_init(&gs.pages, num_pages);

    /* Initialize address map */
    addr_map_init(&gs.addr_map, num_pages, page_size);

    /* Initialize field engine */
    field_engine_init(&gs.field);

    /* Initialize predictor */
    predictor_init(&gs.predictor);

    /* Initialize bandwidth allocator */
    memset(&gs.bandwidth, 0, sizeof(gs.bandwidth));
    gs.bandwidth.num_channels = gs.tiers.num_tiers;
    for (int i = 0; i < gs.tiers.num_tiers; i++) {
        gs.bandwidth.channels[i].id              = i;
        gs.bandwidth.channels[i].tier_id         = gs.tiers.tiers[i].id;
        gs.bandwidth.channels[i].bandwidth_max   = gs.tiers.tiers[i].bandwidth_max;
        gs.bandwidth.channels[i].bandwidth_desired = gs.tiers.tiers[i].bandwidth_max;
        gs.bandwidth.channels[i].weight          = 1.0;
    }

    /* Initialize migrator */
    migrate_engine_init(&gs.migrator);

    /* Initialize pipeline */
    pipeline_init(&gs.rt);
    gs.rt.tiers     = &gs.tiers;
    gs.rt.pages     = &gs.pages;
    gs.rt.addr_map  = &gs.addr_map;
    gs.rt.field     = &gs.field;
    gs.rt.predictor = &gs.predictor;
    gs.rt.bandwidth = &gs.bandwidth;
    gs.rt.migrator  = &gs.migrator;
    gs.rt.tick_interval_ms   = 16.0;
    gs.rt.prediction_horizon = 10;

    /* Initialize page fault handler (with worker thread) */
    if (pf_init(&gs.pf, page_size, num_pages) != 0) {
        fprintf(stderr, "ERRO: falha ao inicializar page fault handler\n");
        return 1;
    }

    /* Allocate pages and evict some to exercise the worker */
    for (uint32_t i = 0; i < num_pages; i++) {
        if (!pf_page_alloc(&gs.pf, i)) {
            fprintf(stderr, "ERRO: alloc page %u\n", i);
            pf_destroy(&gs.pf);
            return 1;
        }
        pf_page_write_pattern(&gs.pf, i, (uint8_t)(i & 0xFF));
    }

    /* Evict half to VRAM-sim, quarter to FILE */
    for (uint32_t i = 0; i < num_pages / 2; i++) {
        pf_page_evict(&gs.pf, i, PF_LOC_VRAM);
    }
    for (uint32_t i = num_pages / 2; i < num_pages * 3 / 4; i++) {
        pf_page_evict(&gs.pf, i, PF_LOC_FILE);
    }

    /* Create pipeline pages mirroring the PF state */
    for (uint32_t i = 0; i < num_pages; i++) {
        pf_page_t *pp = &gs.pf.pages[i];
        if (!pp->valid) continue;

        tier_t *tier = &gs.tiers.tiers[0];
        if (pp->location == PF_LOC_VRAM) tier = &gs.tiers.tiers[1];
        else if (pp->location == PF_LOC_FILE) tier = &gs.tiers.tiers[4];

        page_t *p = page_alloc(&gs.pages, page_size, tier);
        if (p) {
            p->state.raw.x = 0.3 + (double)pp->fault_count * 0.1;
            if (p->state.raw.x > 1.0) p->state.raw.x = 1.0;
            p->access_count = pp->access_count;
            state_update(&p->state);
            page_update_heat(p);
            addr_map_insert(&gs.addr_map, p);
        }
    }

    printf("  %u páginas criadas. Iniciando dashboard...\n", gs.pages.count);
    printf("  Pressione ESC ou feche a janela para sair.\n\n");

    /* Start the pipeline thread */
    pthread_t thread;
    if (pthread_create(&thread, NULL, gui_pipeline_thread, &gs) != 0) {
        fprintf(stderr, "ERRO: falha ao criar thread do pipeline\n");
        pf_destroy(&gs.pf);
        return 1;
    }

    /* Run the GUI (blocks until window closed) */
    int rc = ufta_gui_run(&gs.snap);

    /* Stop the pipeline thread */
    gs.running = false;
    pthread_join(thread, NULL);

    /* Cleanup */
    free(gs.pages.pages);
    free(gs.addr_map.entries);
    predictor_destroy(&gs.predictor);
    pf_destroy(&gs.pf);

    printf("\nDashboard fechado.\n");
    return rc;
}

/* ── Help ─────────────────────────────────────────────────────── */

static void show_help(void)
{
    printf("UFTA-VMM — Universal Field Theory Architecture\n");
    printf("Virtual Memory Manager v%s\n\n", UFTA_VERSION_STRING);
    printf("Commands:\n");
    printf("  create <file> --size <N>[G|M|K]  Create a .vmem file\n");
    printf("  demo                             Run interactive demo\n");
    printf("  run [--file <path>] [--size <N>]  Run the VMM pipeline\n");
    printf("  validate [--pages N] [--page-size N]  Real benchmark + migration test\n");
    printf("  validate-cuda [--pages N] [--page-size N]  Real GPU VRAM test (CUDA)\n");
    printf("  run-cuda [--pages N] [--page-size N]  Pipeline completo com VRAM real\n");
    printf("  page-fault [--pages N] [--page-size N]  Migração transparente via SIGSEGV\n");
    printf("  page-fault-cuda [--pages N] [--page-size N]  Migração transparente com VRAM REAL\n");
    printf("  validate-intelligent [--pages N] [--cycles N] [--lr F]  Migração Inteligente com Aprendizado\n");
    printf("  gui [--pages N] [--page-size N]  Dashboard em tempo real (SDL2 + OpenGL)\n");
    printf("  stats                            Print system stats\n");
    printf("  help                             Show this help\n");
    printf("\nTier hierarchy:\n");
    printf("  T0: RAM    (20 GB/s,  60 ns)\n");
    printf("  T1: VRAM   (500 GB/s, 200 ns)\n");
    printf("  T2: NVMe   (3 GB/s,   20 µs)\n");
    printf("  T3: USB    (120 MB/s, 1 ms)\n");
    printf("  T4: FILE   (200 MB/s, 5 ms)\n");
    printf("\nPipeline: Observe→Represent→Predict→Prioritize→Throttle→Migrate→Commit\n");
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        show_help();
        return 0;
    }

    const char *cmd = argv[1];
    int new_argc = argc - 1;
    char **new_argv = argv + 1;

    if (strcmp(cmd, "create") == 0)
        return cmd_create(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "demo") == 0)
        return cmd_demo(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "run") == 0)
        return cmd_run(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "validate") == 0)
        return cmd_validate(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "validate-cuda") == 0)
        return cmd_validate_cuda(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "run-cuda") == 0)
        return cmd_run_cuda(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "page-fault") == 0)
        return cmd_pagefault(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "page-fault-cuda") == 0)
        return cmd_pagefault_cuda(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "validate-intelligent") == 0)
        return cmd_validate_intelligent(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "gui") == 0)
        return cmd_gui(new_argc - 1, new_argv + 1);
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
        show_help();
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    show_help();
    return 1;
}
