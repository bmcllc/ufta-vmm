/*
 * UFTA-VMM — state.h — State engine: σ = m · σ̂
 *
 * Every entity (page, NPC, region) carries a state vector decomposed
 * into magnitude (intensity) and versor (direction).
 */

#ifndef UFTA_STATE_H
#define UFTA_STATE_H

#include "types.h"

/* ── State vector ─────────────────────────────────────────────── */

typedef struct {
    vec3_t   raw;        /* raw state (h, r, w) — heat, rate, width   */
    real_t   magnitude;  /* m = ||σ||                                 */
    vec3_t   versor;     /* σ̂ = σ / ||σ||  (normalized direction)    */
} state_t;

/* ── Velocity & predicted direction ───────────────────────────── */

typedef struct {
    vec3_t   velocity;      /* v_p: dσ/dt                                */
    vec3_t   dir_predicted; /* d̂_p: predicted future direction          */
    real_t   speed;         /* ||v_p||                                   */
    /* ── New fields for relativistic scheduler ───────────────────── */
    vec3_t   prev_velocity; /* velocity at previous step (for accel) */
    vec3_t   acceleration; /* a_p: d v / dt                            */
    real_t   accel;          /* ||a_p||                                  */
} motion_t;

/* ── Heat level ───────────────────────────────────────────────── */

typedef struct {
    real_t       value;     /* H_p ∈ [0, 1]          */
    heat_level_t level;     /* COLD / WARM / HOT     */
} heat_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Compute magnitude and versor from raw state */
void state_update(state_t *s);

/* Compute heat from raw state: H = h / sqrt(h² + r² + w² + 1) */
heat_t state_heat(const state_t *s);

/* Update velocity from Δstate / Δt */
void motion_update(motion_t *m, const state_t *prev, const state_t *curr, real_t dt);

/* Lerp between two states */
state_t state_lerp(const state_t *a, const state_t *b, real_t t);

#endif /* UFTA_STATE_H */
