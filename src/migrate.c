/*
 * UFTA-VMM — migrate.c — Migration engine with cost/gain analysis
 *
 * G_{i→j} > C_{i→j}  →  migrate
 *
 * G = Σ (c_access^(i) - c_access^(j)) · N̂_p^(k)
 * C = size / B_eff + L_transfer + E_transfer
 */

#include "ufta/migrate.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ── Initialization ───────────────────────────────────────────── */

void migrate_engine_init(migrate_engine_t *me)
{
    memset(me, 0, sizeof(*me));
    me->benefit_threshold = 0.001;  /* minimum net benefit to approve */
    me->energy_weight     = 0.3;
    me->latency_weight    = 0.7;
}

/* ── Cost of access at a tier ─────────────────────────────────── */

real_t tier_access_cost(const tier_t *t, real_t energy_w, real_t latency_w)
{
    return latency_w * t->latency + energy_w * t->energy_cost;
}

/* ── Predicted future accesses (simplified) ───────────────────── */

real_t predicted_accesses(const page_t *p, int horizon)
{
    /* Simple: access_count / time_alive * horizon + velocity component */
    real_t base_rate = (real_t)p->access_count;
    if (base_rate < 1.0) base_rate = 1.0;

    /* Weight by motion speed: faster changing = more predicted accesses */
    real_t motion_factor = 1.0 + vec3_norm(p->motion.velocity) * 0.1;

    return base_rate * motion_factor * (real_t)horizon;
}

/* ── Gain of migrating page from tier_i to tier_j ────────────── */

real_t migrate_compute_gain(const page_t *p, const tier_t *from,
                            const tier_t *to, const migrate_engine_t *me,
                            int horizon)
{
    real_t cost_from = tier_access_cost(from, me->energy_weight, me->latency_weight);
    real_t cost_to   = tier_access_cost(to,   me->energy_weight, me->latency_weight);
    real_t n_pred    = predicted_accesses(p, horizon);

    /* G = (cost_from - cost_to) * predicted_accesses */
    /* If cost_to < cost_from, gain is positive (moving to faster tier helps) */
    return (cost_from - cost_to) * n_pred;
}

/* ── Cost of migration ────────────────────────────────────────── */

real_t migrate_compute_cost(const page_t *p, const tier_t *from,
                            const tier_t *to)
{
    /* C = size / B_eff + L_transfer + E_transfer */
    real_t bandwidth_eff = fmin(from->bandwidth_max, to->bandwidth_max);
    if (bandwidth_eff < 1.0) bandwidth_eff = 1.0;

    real_t transfer_time = (real_t)p->size / bandwidth_eff;
    real_t latency       = from->latency + to->latency;
    real_t energy        = from->energy_cost + to->energy_cost;

    return transfer_time + latency + energy;
}

/* ── Evaluate migration decision ──────────────────────────────── */

migrate_decision_t migrate_evaluate(const page_t *p, const tier_t *from,
                                    const tier_t *to,
                                    const migrate_engine_t *me, int horizon)
{
    migrate_decision_t d;
    memset(&d, 0, sizeof(d));

    d.page_id  = p->id;
    d.from_tier = from->id;
    d.to_tier   = to->id;

    if (p->pinned) {
        d.action    = MIGRATE_NONE;
        d.approved  = false;
        return d;
    }

    d.gain = migrate_compute_gain(p, from, to, me, horizon);
    d.cost = migrate_compute_cost(p, from, to);
    d.net_benefit = d.gain - d.cost;

    /* Determine action direction */
    if (to->id < from->id) {
        d.action = MIGRATE_UP;    /* to faster tier */
    } else if (to->id > from->id) {
        d.action = MIGRATE_DOWN;  /* to slower tier */
    } else {
        d.action = MIGRATE_NONE;
    }

    /* Approve if net benefit exceeds threshold */
    d.approved = (d.net_benefit > me->benefit_threshold);

    return d;
}

/* ── Enqueue migration ────────────────────────────────────────── */

int migrate_enqueue(migrate_engine_t *me, const migrate_decision_t *d)
{
    if (me->queue_len >= UFTA_MAX_MIGRATE_QUEUE)
        return UFTA_ERR_FULL;

    me->queue[me->queue_len++] = *d;
    return UFTA_OK;
}

/* ── Execute pending migrations ───────────────────────────────── */

int migrate_execute(migrate_engine_t *me, page_table_t *pt,
                    tier_registry_t *registry)
{
    int executed = 0;

    for (int i = 0; i < me->queue_len; i++) {
        migrate_decision_t *d = &me->queue[i];

        if (!d->approved) {
            me->stats.skipped_no_gain++;
            continue;
        }

        page_t *p = page_find(pt, d->page_id);
        if (!p) continue;

        if (p->pinned) {
            me->stats.skipped_pinned++;
            continue;
        }

        /* Check if destination tier has space */
        tier_t *to_tier = &registry->tiers[d->to_tier];
        if (to_tier->capacity != UINT64_MAX &&
            to_tier->used + p->size > to_tier->capacity) {
            me->stats.skipped_full++;
            continue;
        }

        /* Perform migration: update tier pointers and stats */
        tier_t *from_tier = p->tier_ptr;

        if (from_tier) {
            if (from_tier->used >= p->size)
                from_tier->used -= p->size;
            if (from_tier->page_count > 0)
                from_tier->page_count--;
            from_tier->utilization =
                (real_t)from_tier->used / (real_t)from_tier->capacity;
        }

        p->tier_id  = d->to_tier;
        p->tier_ptr = to_tier;
        p->migrate_count++;

        to_tier->used += p->size;
        to_tier->page_count++;
        to_tier->utilization =
            (real_t)to_tier->used / (real_t)to_tier->capacity;

        me->stats.total_migrations++;
        me->stats.successful++;
        me->stats.total_bytes_moved += p->size;
        executed++;
    }

    /* Clear queue */
    me->queue_len = 0;
    return executed;
}

/* ── Query ────────────────────────────────────────────────────── */

bool migrate_is_beneficial(const migrate_decision_t *d)
{
    return d->approved && d->net_benefit > 0;
}

/* ── Print stats ──────────────────────────────────────────────── */

void migrate_stats_print(const migrate_engine_t *me, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;
    fprintf(f, "=== Migration Stats ===\n");
    fprintf(f, "  Total migrations:  %llu\n",
            (unsigned long long)me->stats.total_migrations);
    fprintf(f, "  Successful:        %llu\n",
            (unsigned long long)me->stats.successful);
    fprintf(f, "  Skipped (no gain): %llu\n",
            (unsigned long long)me->stats.skipped_no_gain);
    fprintf(f, "  Skipped (pinned):  %llu\n",
            (unsigned long long)me->stats.skipped_pinned);
    fprintf(f, "  Skipped (full):    %llu\n",
            (unsigned long long)me->stats.skipped_full);
    fprintf(f, "  Bytes moved:       %.1f MB\n",
            me->stats.total_bytes_moved / (1024.0 * 1024.0));
    fclose(f);
}
