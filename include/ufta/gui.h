/*
 * UFTA-VMM — gui.h — Real-time monitoring dashboard (SDL2 + OpenGL)
 *
 * A lightweight, self-contained GUI that shows the commercial-facing
 * metrics in real time:
 *
 *   • VRAM recuperada (bytes liberados / migrados)
 *   • Redução de latência (µs por page fault)
 *   • Estabilidade de FPS (tempo de ciclo do pipeline)
 *   • Utilização dos tiers (barras)
 *   • Estatísticas de migração
 *   • Estatísticas da worker thread (batching)
 *
 * Built with SDL2 + OpenGL (immediate-mode 2D rendering) — no external
 * GUI framework required, keeping the product plug-and-play.
 */

#ifndef UFTA_GUI_H
#define UFTA_GUI_H

#include "types.h"
#include "tier.h"
#include "migrate.h"
#include "pipeline.h"
#include "worker.h"
#include "pagefault.h"

/* ── GUI configuration ────────────────────────────────────────── */

#define UFTA_GUI_WINDOW_W   1280
#define UFTA_GUI_WINDOW_H   720
#define UFTA_GUI_TITLE      "UFTA-VMM Dashboard"
#define UFTA_GUI_MAX_SAMPLES 600   /* history points for charts */

/* ── Snapshot of live metrics fed to the GUI each frame ───────── */

typedef struct {
    /* Tier utilization */
    tier_registry_t *tiers;

    /* Pipeline metrics */
    const pipeline_metrics_t *metrics;
    uint64_t cycle_count;

    /* Migration stats */
    const migrate_stats_t *migrate;

    /* Worker thread stats */
    const ufta_worker_t *worker;

    /* Page fault stats */
    const pf_context_t *pf;

    /* Derived / computed values */
    double vram_recovered_mb;   /* bytes moved to VRAM (MB) */
    double latency_us_per_fault;/* avg µs per fault */
    double fps_estimate;        /* 1e6 / cycle_us */
} ufta_gui_snapshot_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Run the GUI dashboard loop. Blocks until the window is closed.
 * Returns 0 on success, -1 on error. */
int ufta_gui_run(const ufta_gui_snapshot_t *snap);

#endif /* UFTA_GUI_H */
