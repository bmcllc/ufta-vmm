/*
 * UFTA-VMM — tier.h — Memory tier definition
 *
 * T_i = (B_max, L, C, E, Q)
 */

#ifndef UFTA_TIER_H
#define UFTA_TIER_H

#include "types.h"

#define UFTA_MAX_TIERS      8
#define UFTA_MAX_CHANNELS   16

/* ── Memory tier ──────────────────────────────────────────────── */

typedef struct {
    tier_id_t id;
    char      name[32];

    real_t    bandwidth_max;   /* B_max: bytes/s (theoretical) */
    real_t    latency;         /* L: seconds (theoretical)     */
    uint64_t  capacity;        /* C: bytes                     */
    real_t    energy_cost;     /* E: J/op                      */
    real_t    transfer_cost;   /* Q: overhead factor           */

    /* Runtime stats */
    uint64_t  used;            /* bytes currently used */
    uint32_t  page_count;      /* pages resident       */
    real_t    utilization;     /* used / capacity      */

    /* ── Measured bandwidth (real) ─────────────────────────── */
    real_t    bw_measured;     /* B_measured: bytes/s from benchmark */
    real_t    lat_measured;    /* L_measured: seconds from benchmark  */
    bool      calibrated;      /* has real measurement been done?     */

    /* Transfer counters (bytes moved through this tier) */
    uint64_t  bytes_read;
    uint64_t  bytes_written;
    uint64_t  bytes_migrated;
    uint64_t  transfer_count;
} tier_t;

/* ── Channel (bandwidth budget) ──────────────────────────────── */

typedef struct {
    chan_id_t  id;
    tier_id_t tier_id;
    real_t    bandwidth_max;   /* B_j^max: physical limit    */
    real_t    bandwidth_desired; /* β_j(t): desired          */
    real_t    bandwidth_budget;  /* B_j^budget(t): granted   */
    real_t    bandwidth_alloc;   /* B_j^alloc(t): effective  */
    real_t    weight;            /* w_j(t): dynamic priority  */
} channel_t;

/* ── Bandwidth allocator ─────────────────────────────────────── */

typedef struct {
    channel_t channels[UFTA_MAX_CHANNELS];
    int       num_channels;
    real_t    total_budget;    /* B^total(t) */
} bw_allocator_t;

/* ── Tier Registry ───────────────────────────────────────────── */

typedef struct {
    tier_t  tiers[UFTA_MAX_TIERS];
    int     num_tiers;
} tier_registry_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize tier with parameters */
void tier_init(tier_t *t, const char *name, real_t bw_max, real_t latency,
               uint64_t capacity, real_t energy, real_t transfer);

/* Create default tier registry with common backends */
void tier_registry_init_defaults(tier_registry_t *reg);

/* Find tier by name, returns NULL if not found */
tier_t *tier_registry_find(tier_registry_t *reg, const char *name);

/* Compute effective bandwidth: min(B_max, β, B_budget) */
real_t bw_effective(const channel_t *ch);

/* Allocate bandwidth budget across channels (weighted optimization) */
void bw_allocate(bw_allocator_t *alloc);

/* Print tier info to file descriptor */
void tier_print(const tier_t *t, int fd);

#endif /* UFTA_TIER_H */
