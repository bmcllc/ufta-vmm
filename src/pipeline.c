/*
 * UFTA-VMM — pipeline.c — Runtime pipeline: the 7-step cycle
 *
 * Observe → Represent → Predict → Prioritize → Throttle → Migrate → Commit
 */

#include "ufta/pipeline.h"
#include "ufta/platform.h"
#include <string.h>
#include <stdio.h>
#include <time.h>


/* ── High-resolution timestamp ────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static real_t elapsed_us(uint64_t start, uint64_t end)
{
    return (real_t)(end - start) / 1000.0;
}

/* ══════════════════════════════════════════════════════════════════
 * Pipeline Steps
 * ══════════════════════════════════════════════════════════════════ */

/* Step 1: Observe — collect metrics */
void pipeline_observe(runtime_t *rt)
{
    uint64_t t0 = now_ns();

    /* Update tier utilization stats */
    for (int i = 0; i < rt->tiers->num_tiers; i++) {
        tier_t *t = &rt->tiers->tiers[i];
        t->utilization = (t->capacity == UINT64_MAX) ? 0.0 :
                         (real_t)t->used / (real_t)t->capacity;
    }

    /* Mark pages that haven't been accessed as potentially cold */
    for (uint32_t i = 0; i < rt->pages->count; i++) {
        page_t *p = &rt->pages->pages[i];

        /* Decay heat if not accessed recently */
        uint64_t age = rt->timestamp_ns - p->last_access_ts;
        if (age > 1000000000ULL) { /* > 1 second */
            p->state.raw.x *= 0.95; /* heat decay */
            if (p->state.raw.x < 0.01) p->state.raw.x = 0.01;
        }
    }

    rt->metrics.duration_us[STEP_OBSERVE] = elapsed_us(t0, now_ns());
}

/* Step 2: Represent — update σ, v, d̂ */
void pipeline_represent(runtime_t *rt)
{
    uint64_t t0 = now_ns();

    for (uint32_t i = 0; i < rt->pages->count; i++) {
        page_t *p = &rt->pages->pages[i];
        page_update_state(p);
        page_update_heat(p);
    }

    rt->metrics.duration_us[STEP_REPRESENT] = elapsed_us(t0, now_ns());
}

/* Step 3: Predict — compute σ̂_{t+Δt}^{pred} */
void pipeline_predict(runtime_t *rt)
{
    uint64_t t0 = now_ns();
    real_t dt = rt->tick_interval_ms / 1000.0;

    for (uint32_t i = 0; i < rt->pages->count; i++) {
        page_t *p = &rt->pages->pages[i];

        /* Ensure predictor has state for this page */
        pred_state_t *ps = predictor_get(rt->predictor, p->id);
        if (!ps) continue;

        /* LEARN: compare last tick's prediction (σ̂_{t}) against the
         * actual current state (σ_t). This is the 1-step-ahead error
         * that drives LMS gradient descent.
         *
         * We learn when predictor_step has been called at least once
         * before (last_dt > 0 means we have a stored prediction context). */
        if (ps->last_dt > 0.0) {
            predictor_lms_update(ps, &p->state.versor,
                                 rt->predictor->learning_rate,
                                 rt->predictor->momentum);
        }

        /* Compute predicted direction for next step: σ̂_{t+1} */
        vec3_t predicted = predictor_step(rt->predictor, p, dt);
        p->motion.dir_predicted = predicted;
    }

    rt->metrics.duration_us[STEP_PREDICT] = elapsed_us(t0, now_ns());
}

/* Step 4: Prioritize — compute priority π_p for all pages */
void pipeline_prioritize(runtime_t *rt)
{
    uint64_t t0 = now_ns();

    /* Update bandwidth weights based on page priorities */
    for (int c = 0; c < rt->bandwidth->num_channels; c++) {
        channel_t *ch = &rt->bandwidth->channels[c];
        real_t total_weight = 0.0;

        /* Sum heat of pages on this channel's tier */
        for (uint32_t i = 0; i < rt->pages->count; i++) {
            page_t *p = &rt->pages->pages[i];
            if (p->tier_id == ch->tier_id) {
                total_weight += p->heat.value;
            }
        }

        ch->weight = total_weight;
    }

    rt->metrics.duration_us[STEP_PRIORITIZE] = elapsed_us(t0, now_ns());
}

/* Step 5: Throttle — allocate bandwidth */
void pipeline_throttle(runtime_t *rt)
{
    uint64_t t0 = now_ns();

    /* Compute total available bandwidth */
    rt->bandwidth->total_budget = 0;
    for (int i = 0; i < rt->tiers->num_tiers; i++) {
        rt->bandwidth->total_budget += rt->tiers->tiers[i].bandwidth_max;
    }

    /* Run bandwidth allocator */
    bw_allocate(rt->bandwidth);

    rt->metrics.duration_us[STEP_THROTTLE] = elapsed_us(t0, now_ns());
}

/* Step 6: Migrate — evaluate and execute migrations */
void pipeline_migrate(runtime_t *rt)
{
    uint64_t t0 = now_ns();

    /* Evaluate migration for each page against better tiers */
    for (uint32_t i = 0; i < rt->pages->count; i++) {
        page_t *p = &rt->pages->pages[i];
        if (p->pinned) continue;

        tier_t *current = &rt->tiers->tiers[p->tier_id];

        /* Determine effective heat: use access_count as a secondary signal.
         * A page with high access_count is hot even if instantaneous heat
         * (from raw.x) is momentarily low due to pattern oscillation. */
        bool is_hot = (p->heat.level == HEAT_HOT) || (p->access_count > 50);
        bool is_cold = (p->heat.level == HEAT_COLD) && (p->access_count < 10);

        /* Try migrating to faster tier if hot */
        if (is_hot && p->tier_id > 0) {
            tier_t *faster = &rt->tiers->tiers[p->tier_id - 1];
            migrate_decision_t d = migrate_evaluate(
                p, current, faster, rt->migrator, rt->prediction_horizon);
            if (d.approved) {
                migrate_enqueue(rt->migrator, &d);
            }
        }

        /* Try migrating to slower tier if cold */
        if (is_cold &&
            p->tier_id < (tier_id_t)(rt->tiers->num_tiers - 1)) {
            tier_t *slower = &rt->tiers->tiers[p->tier_id + 1];
            migrate_decision_t d = migrate_evaluate(
                p, current, slower, rt->migrator, rt->prediction_horizon);
            if (d.approved) {
                migrate_enqueue(rt->migrator, &d);
            }
        }
    }

    /* Execute queued migrations */
    int count = migrate_execute(rt->migrator, rt->pages, rt->tiers);
    rt->metrics.migrations_this_cycle = count;

    rt->metrics.duration_us[STEP_MIGRATE] = elapsed_us(t0, now_ns());
}

/* Step 7: Commit — apply state changes */
void pipeline_commit(runtime_t *rt)
{
    uint64_t t0 = now_ns();

    /* Apply field engine evolution to all pages */
    real_t dt = rt->tick_interval_ms / 1000.0;
    for (uint32_t i = 0; i < rt->pages->count; i++) {
        page_t *p = &rt->pages->pages[i];
        field_engine_step_page(rt->field, p, dt);
    }

    /* Update predictor with new observations (learning happens in
     * pipeline_predict via 1-step-ahead comparison) */
    for (uint32_t i = 0; i < rt->pages->count; i++) {
        page_t *p = &rt->pages->pages[i];
        predictor_get(rt->predictor, p->id);
    }

    rt->metrics.pages_processed = rt->pages->count;
    rt->metrics.duration_us[STEP_COMMIT] = elapsed_us(t0, now_ns());
}

/* ══════════════════════════════════════════════════════════════════
 * Pipeline Runner
 * ══════════════════════════════════════════════════════════════════ */

void pipeline_init(runtime_t *rt)
{
    memset(rt, 0, sizeof(*rt));
    rt->tick_interval_ms    = 16.0;  /* ~60 Hz */
    rt->prediction_horizon  = 10;
    rt->running             = false;
    rt->cycle_count         = 0;
    rt->timestamp_ns        = now_ns();
}

void pipeline_tick(runtime_t *rt)
{
    uint64_t cycle_start = now_ns();
    rt->timestamp_ns = cycle_start;

    pipeline_observe(rt);
    pipeline_represent(rt);
    pipeline_predict(rt);
    pipeline_prioritize(rt);
    pipeline_throttle(rt);
    pipeline_migrate(rt);
    pipeline_commit(rt);

    rt->cycle_count++;
    rt->metrics.total_cycle_us = elapsed_us(cycle_start, now_ns());
}

void pipeline_run(runtime_t *rt)
{
    rt->running = true;
    while (rt->running) {
        pipeline_tick(rt);

        /* Sleep for tick interval */
        struct timespec ts;
        ts.tv_sec  = (time_t)(rt->tick_interval_ms / 1000.0);
        ts.tv_nsec = (long)((rt->tick_interval_ms / 1000.0 - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
}

void pipeline_metrics_print(const runtime_t *rt, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;

    fprintf(f, "=== Pipeline Cycle #%llu ===\n",
            (unsigned long long)rt->cycle_count);
    fprintf(f, "  Total cycle: %.1f µs\n", rt->metrics.total_cycle_us);
    fprintf(f, "  Pages processed: %llu\n",
            (unsigned long long)rt->metrics.pages_processed);
    fprintf(f, "  Migrations this cycle: %llu\n",
            (unsigned long long)rt->metrics.migrations_this_cycle);

    const char *names[] = {"Observe","Represent","Predict","Prioritize",
                           "Throttle","Migrate","Commit"};
    for (int i = 0; i < STEP_COUNT; i++) {
        fprintf(f, "  %-12s: %8.1f µs\n", names[i],
                rt->metrics.duration_us[i]);
    }
    fclose(f);
}
