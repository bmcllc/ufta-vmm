/*
 * UFTA-VMM — migrate.h — Migration engine with cost/gain analysis
 *
 * G_{i→j}(p) > C_{i→j}(p)  →  migrate
 *
 * G = Σ (c_access^(i) - c_access^(j)) · N̂_p^(k)
 * C = size_p / B_eff + L_transfer + E_transfer
 */

#ifndef UFTA_MIGRATE_H
#define UFTA_MIGRATE_H

#include "types.h"
#include "page.h"
#include "tier.h"

/* ── Migration decision ───────────────────────────────────────── */

typedef struct {
    page_id_t       page_id;
    tier_id_t       from_tier;
    tier_id_t       to_tier;
    real_t          gain;         /* G_{i→j} */
    real_t          cost;         /* C_{i→j} */
    real_t          net_benefit;  /* G - C   */
    migrate_action_t action;
    bool            approved;    /* passes threshold? */
} migrate_decision_t;

/* ── Migration statistics ─────────────────────────────────────── */

typedef struct {
    uint64_t  total_migrations;
    uint64_t  successful;
    uint64_t  skipped_no_gain;
    uint64_t  skipped_pinned;
    uint64_t  skipped_full;
    real_t    total_bytes_moved;
    real_t    total_time_spent;     /* seconds */
    real_t    avg_latency;          /* seconds per migration */
} migrate_stats_t;

/* ── Migration engine ─────────────────────────────────────────── */

#define UFTA_MAX_MIGRATE_QUEUE  1024

typedef struct {
    migrate_decision_t queue[UFTA_MAX_MIGRATE_QUEUE];
    int                queue_len;

    real_t             benefit_threshold;  /* min net_benefit to approve */
    real_t             energy_weight;      /* importance of energy in G  */
    real_t             latency_weight;     /* importance of latency in G */

    migrate_stats_t    stats;
} migrate_engine_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize migration engine */
void migrate_engine_init(migrate_engine_t *me);

/* Compute cost of access at a given tier */
real_t tier_access_cost(const tier_t *t, real_t energy_w, real_t latency_w);

/* Compute predicted future accesses from predictor */
real_t predicted_accesses(const page_t *p, int horizon);

/* Compute gain G_{i→j} for migrating page p */
real_t migrate_compute_gain(const page_t *p, const tier_t *from,
                            const tier_t *to, const migrate_engine_t *me,
                            int horizon);

/* Compute migration cost C_{i→j} */
real_t migrate_compute_cost(const page_t *p, const tier_t *from,
                            const tier_t *to);

/* Evaluate migration for a single page, populate decision */
migrate_decision_t migrate_evaluate(const page_t *p, const tier_t *from,
                                    const tier_t *to,
                                    const migrate_engine_t *me, int horizon);

/* Enqueue a migration decision */
int migrate_enqueue(migrate_engine_t *me, const migrate_decision_t *d);

/* Execute pending migrations from the queue */
int migrate_execute(migrate_engine_t *me, page_table_t *pt,
                    tier_registry_t *registry);

/* Check if migration is beneficial */
bool migrate_is_beneficial(const migrate_decision_t *d);

/* Print migration stats */
void migrate_stats_print(const migrate_engine_t *me, int fd);

#endif /* UFTA_MIGRATE_H */
