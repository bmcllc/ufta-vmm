/*
 * UFTA-VMM — pipeline.h — Runtime pipeline: the 7-step cycle
 *
 * Observe → Represent → Predict → Prioritize → Throttle → Migrate → Commit
 */

#ifndef UFTA_PIPELINE_H
#define UFTA_PIPELINE_H

#include "types.h"
#include "state.h"
#include "tier.h"
#include "page.h"
#include "field.h"
#include "predictor.h"
#include "migrate.h"

/* ── Pipeline step identifiers ────────────────────────────────── */

typedef enum {
    STEP_OBSERVE    = 0,
    STEP_REPRESENT  = 1,
    STEP_PREDICT    = 2,
    STEP_PRIORITIZE = 3,
    STEP_THROTTLE   = 4,
    STEP_MIGRATE    = 5,
    STEP_COMMIT     = 6,
    STEP_COUNT      = 7
} pipeline_step_t;

/* ── Pipeline metrics ─────────────────────────────────────────── */

typedef struct {
    real_t duration_us[STEP_COUNT]; /* per-step timing in µs */
    uint64_t pages_processed;
    uint64_t migrations_this_cycle;
    real_t total_cycle_us;
} pipeline_metrics_t;

/* ── Runtime context (all engine references) ──────────────────── */

typedef struct {
    tier_registry_t  *tiers;
    page_table_t     *pages;
    addr_map_t       *addr_map;
    field_engine_t   *field;
    predictor_t      *predictor;
    bw_allocator_t   *bandwidth;
    migrate_engine_t *migrator;

    pipeline_metrics_t metrics;

    /* Timing */
    uint64_t cycle_count;
    uint64_t timestamp_ns;

    /* Configuration */
    real_t   tick_interval_ms;   /* main loop interval */
    int      prediction_horizon; /* steps ahead to predict */
    bool     running;
} runtime_t;

/* ── Step function prototypes ─────────────────────────────────── */

/* Step 1: Observe — collect metrics (access counts, latencies, dirty bits) */
void pipeline_observe(runtime_t *rt);

/* Step 2: Represent — update σ, v, d̂ for all pages */
void pipeline_represent(runtime_t *rt);

/* Step 3: Predict — compute σ̂_{t+Δt}^{pred} for all pages */
void pipeline_predict(runtime_t *rt);

/* Step 4: Prioritize — compute π_p (priority) for all pages */
void pipeline_prioritize(runtime_t *rt);

/* Step 5: Throttle — allocate bandwidth B_j^{alloc}(t) */
void pipeline_throttle(runtime_t *rt);

/* Step 6: Migrate — evaluate and execute migrations */
void pipeline_migrate(runtime_t *rt);

/* Step 7: Commit — apply state changes, persist dirty pages */
void pipeline_commit(runtime_t *rt);

/* ── Pipeline runner ──────────────────────────────────────────── */

/* Initialize runtime with all engines */
void pipeline_init(runtime_t *rt);

/* Execute one full cycle */
void pipeline_tick(runtime_t *rt);

/* Run pipeline until stopped */
void pipeline_run(runtime_t *rt);

/* Print metrics for last cycle */
void pipeline_metrics_print(const runtime_t *rt, int fd);

#endif /* UFTA_PIPELINE_H */
