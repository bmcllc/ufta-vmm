/*
 * UFTA-VMM — predictor.h — Adaptive prediction with error
 *
 * σ̂_{t+Δt} = (σ_t + R(θ) · v_t · Δt) / ||...||
 *
 * E_p(t) = ||σ̂(t+τ) - σ̂_pred(t+τ)||
 * θ ← θ - η · ∇_θ E_p(t)
 */

#ifndef UFTA_PREDICTOR_H
#define UFTA_PREDICTOR_H

#include "types.h"
#include "state.h"
#include "page.h"

/* ── Prediction state per page ────────────────────────────────── */

typedef struct {
    page_id_t page_id;

    mat3_t    rotation;       /* R(θ): rotation model        */
    real_t    theta[3];       /* rotation parameters          */
    real_t    theta_vel[3];   /* momentum accumulator for θ   */
    real_t    error;          /* E_p(t): accumulated error    */
    real_t    error_ema;      /* exponential moving avg       */

    vec3_t    predicted_dir;  /* d̂_pred(t+Δt)                */
    real_t    confidence;     /* ∈ [0,1]: prediction quality  */

    /* Stored context for gradient computation */
    vec3_t    last_state;     /* σ_t at last predict step    */
    vec3_t    last_velocity;  /* v_t at last predict step    */
    real_t    last_dt;        /* dt at last predict step     */

    /* History ring buffer for gradient estimation */
    #define PRED_HIST_LEN 16
    vec3_t    hist_sigma[PRED_HIST_LEN];
    vec3_t    hist_predicted[PRED_HIST_LEN];
    uint32_t  hist_idx;
    uint32_t  hist_count;

    /* Learning stats */
    uint64_t  total_updates;  /* number of gradient updates   */
    real_t    total_error;    /* cumulative error for avg     */
} pred_state_t;

/* ── Predictor engine ─────────────────────────────────────────── */

#define UFTA_MAX_PRED_PAGES  65536

typedef struct {
    pred_state_t  *states;      /* heap-allocated array */
    int           num_states;
    int           capacity;

    real_t        learning_rate;   /* η: gradient step size    */
    real_t        momentum;        /* momentum for θ update    */
    real_t        default_confidence;
    real_t        avg_error;       /* global avg prediction error */
    uint64_t      total_updates;   /* global update counter     */
} predictor_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize predictor */
void predictor_init(predictor_t *pred);

/* Free predictor resources */
void predictor_destroy(predictor_t *pred);

/* Get or create prediction state for a page */
pred_state_t *predictor_get(predictor_t *pred, page_id_t page_id);

/* Predict future direction: σ̂_{t+Δt}^{pred} */
vec3_t predictor_step(const predictor_t *pred, const page_t *p, real_t dt);

/* Update predictor with observed outcome (adjusts θ via gradient) */
void predictor_update(predictor_t *pred, page_id_t page_id,
                      const vec3_t *observed, real_t dt);

/* Get prediction error for a page */
real_t predictor_error(const predictor_t *pred, page_id_t page_id);

/* Get prediction confidence for a page */
real_t predictor_confidence(const predictor_t *pred, page_id_t page_id);

/* Build rotation matrix from theta parameters */
mat3_t build_rotation(const real_t theta[3]);

/* LMS-style gradient update for rotation parameters with momentum */
void predictor_lms_update(pred_state_t *ps, const vec3_t *observed,
                          real_t learning_rate, real_t momentum);

/* ── Relativistic Scheduler ───────────────────────────────────── */

/* Compute Lorentz factor γ = 1 / sqrt(1 - β²) where β = v / v_max */
real_t lorentz_factor(real_t speed, real_t max_speed);

/* Compute relativistic priority: P_i = γ_i · R_i · G_i - C_i */
real_t relativistic_priority(const pred_state_t *ps, real_t heat, real_t max_speed);

#endif /* UFTA_PREDICTOR_H */
