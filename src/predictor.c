/*
 * UFTA-VMM — predictor.c — Adaptive prediction with error
 *
 * σ̂_{t+Δt} = (σ_t + R(θ) · v_t · Δt) / ||...||
 * θ ← θ - η · ∇_θ E_p(t)   (LMS-style update)
 */

#include "ufta/predictor.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── Build rotation matrix from Euler-like parameters ─────────── */

mat3_t build_rotation(const real_t theta[3])
{
    real_t cx = cos(theta[0]), sx = sin(theta[0]);
    real_t cy = cos(theta[1]), sy = sin(theta[1]);
    real_t cz = cos(theta[2]), sz = sin(theta[2]);

    mat3_t Rx = mat3_identity();
    Rx.m[1][1] =  cx; Rx.m[1][2] = -sx;
    Rx.m[2][1] =  sx; Rx.m[2][2] =  cx;

    mat3_t Ry = mat3_identity();
    Ry.m[0][0] =  cy; Ry.m[0][2] =  sy;
    Ry.m[2][0] = -sy; Ry.m[2][2] =  cy;

    mat3_t Rz = mat3_identity();
    Rz.m[0][0] =  cz; Rz.m[0][1] = -sz;
    Rz.m[1][0] =  sz; Rz.m[1][1] =  cz;

    return mat3_mul(Rz, mat3_mul(Ry, Rx));
}

/* ── Predictor initialization ─────────────────────────────────── */

void predictor_init(predictor_t *pred)
{
    memset(pred, 0, sizeof(*pred));
    pred->learning_rate     = 0.01;
    pred->momentum          = 0.9;
    pred->default_confidence = 0.5;
    pred->num_states        = 0;
    pred->capacity          = 0;
    pred->states            = NULL;
}

void predictor_destroy(predictor_t *pred)
{
    free(pred->states);
    pred->states   = NULL;
    pred->capacity = 0;
    pred->num_states = 0;
}

/* ── Get or create prediction state for a page ────────────────── */

pred_state_t *predictor_get(predictor_t *pred, page_id_t page_id)
{
    /* Search existing */
    for (int i = 0; i < pred->num_states; i++) {
        if (pred->states[i].page_id == page_id)
            return &pred->states[i];
    }

    /* Grow array if needed */
    if (pred->num_states >= pred->capacity) {
        int new_cap = pred->capacity == 0 ? 256 : pred->capacity * 2;
        if (new_cap > UFTA_MAX_PRED_PAGES) new_cap = UFTA_MAX_PRED_PAGES;
        if (pred->num_states >= new_cap)
            return NULL;

        pred_state_t *new_states = realloc(pred->states,
                                           (size_t)new_cap * sizeof(pred_state_t));
        if (!new_states) return NULL;
        pred->states   = new_states;
        pred->capacity = new_cap;
    }

    pred_state_t *ps = &pred->states[pred->num_states++];
    memset(ps, 0, sizeof(*ps));
    ps->page_id   = page_id;
    ps->rotation  = mat3_identity();
    ps->theta[0]  = ps->theta[1] = ps->theta[2] = 0.0;
    ps->theta_vel[0] = ps->theta_vel[1] = ps->theta_vel[2] = 0.0;
    ps->error     = 0.0;
    ps->error_ema = 0.0;
    ps->confidence = pred->default_confidence;
    ps->total_updates = 0;
    ps->total_error   = 0.0;

    return ps;
}

/* ── Predict future direction ─────────────────────────────────── */

vec3_t predictor_step(const predictor_t *pred, const page_t *p, real_t dt)
{
    /* Find prediction state */
    const pred_state_t *ps = NULL;
    for (int i = 0; i < pred->num_states; i++) {
        if (pred->states[i].page_id == p->id) {
            ps = &pred->states[i];
            break;
        }
    }

    if (!ps) {
        /* No prediction model: return current versor */
        return p->state.versor;
    }

    /* Apply rotation to velocity */
    vec3_t rotated_v = mat3_mul_vec(ps->rotation, p->motion.velocity);

    /* Predict: σ̂_{t+Δt} = (σ_t + R(θ) · v_t · Δt) / ||...|| */
    vec3_t predicted = vec3_add(p->state.raw,
                                vec3_scale(rotated_v, dt));
    vec3_t predicted_dir = vec3_normalize(predicted);

    /* Store context for gradient computation in lms_update */
    pred_state_t *ps_mut = (pred_state_t *)ps; /* safe: we only write non-const fields */
    ps_mut->predicted_dir = predicted_dir;
    ps_mut->last_state    = p->state.raw;
    ps_mut->last_velocity = p->motion.velocity;
    ps_mut->last_dt       = dt;

    return predicted_dir;
}

/* ── LMS-style gradient update with momentum ──────────────────── */

void predictor_lms_update(pred_state_t *ps, const vec3_t *observed,
                          real_t learning_rate, real_t momentum)
{
    /* Compute error: E = ||observed - predicted|| */
    vec3_t err_vec = vec3_sub(*observed, ps->predicted_dir);
    real_t err_mag = vec3_norm(err_vec);

    if (err_mag < 1e-12) return;

    /* Finite-difference gradient for each theta component
     *
     * We recompute the full prediction σ̂(θ_i ± ε) = normalize(σ_t + R(θ_i±ε)·v·dt)
     * and measure ∂E/∂θ_i ≈ (E(θ_i+ε) - E(θ_i-ε)) / 2ε
     */
    real_t epsilon = 1e-4;
    real_t saved_theta[3] = {ps->theta[0], ps->theta[1], ps->theta[2]};

    for (int i = 0; i < 3; i++) {
        /* E_plus: error with theta_i + epsilon */
        ps->theta[i] = saved_theta[i] + epsilon;
        mat3_t R_plus = build_rotation(ps->theta);
        vec3_t pred_plus = vec3_add(ps->last_state,
            vec3_scale(mat3_mul_vec(R_plus, ps->last_velocity), ps->last_dt));
        real_t err_plus = vec3_norm(vec3_sub(*observed, vec3_normalize(pred_plus)));

        /* E_minus: error with theta_i - epsilon */
        ps->theta[i] = saved_theta[i] - epsilon;
        mat3_t R_minus = build_rotation(ps->theta);
        vec3_t pred_minus = vec3_add(ps->last_state,
            vec3_scale(mat3_mul_vec(R_minus, ps->last_velocity), ps->last_dt));
        real_t err_minus = vec3_norm(vec3_sub(*observed, vec3_normalize(pred_minus)));

        /* Gradient: ∂E/∂θ_i ≈ (E_plus - E_minus) / 2ε */
        real_t grad = (err_plus - err_minus) / (2.0 * epsilon);

        /* Momentum update: v_i = momentum * v_i - η * grad */
        ps->theta_vel[i] = momentum * ps->theta_vel[i] - learning_rate * grad;

        /* Apply: θ_i += v_i */
        ps->theta[i] = saved_theta[i] + ps->theta_vel[i];

        /* Wrap to [-π, π] */
        while (ps->theta[i] >  M_PI) ps->theta[i] -= 2.0 * M_PI;
        while (ps->theta[i] < -M_PI) ps->theta[i] += 2.0 * M_PI;
    }

    /* Update rotation matrix from new theta */
    ps->rotation = build_rotation(ps->theta);

    /* Update error EMA (α=0.1 for smooth tracking) */
    ps->error = err_mag;
    ps->error_ema = 0.9 * ps->error_ema + 0.1 * err_mag;

    /* Update confidence: sigmoid decay from error */
    ps->confidence = 1.0 / (1.0 + ps->error_ema * 10.0);

    /* Track stats */
    ps->total_updates++;
    ps->total_error += err_mag;
}

/* ── Update predictor with observed outcome ───────────────────── */

void predictor_update(predictor_t *pred, page_id_t page_id,
                      const vec3_t *observed, real_t dt)
{
    (void)dt;
    pred_state_t *ps = predictor_get((predictor_t *)pred, page_id);
    if (!ps) return;

    predictor_lms_update(ps, observed, pred->learning_rate, pred->momentum);

    /* Store in history ring buffer */
    uint32_t idx = ps->hist_idx % PRED_HIST_LEN;
    ps->hist_sigma[idx]     = *observed;
    ps->hist_predicted[idx] = ps->predicted_dir;
    ps->hist_idx++;
    if (ps->hist_count < PRED_HIST_LEN) ps->hist_count++;

    /* Update global stats */
    pred->total_updates++;
    pred->avg_error = 0.999 * pred->avg_error + 0.001 * ps->error_ema;
}

/* ── Query functions ──────────────────────────────────────────── */

real_t predictor_error(const predictor_t *pred, page_id_t page_id)
{
    for (int i = 0; i < pred->num_states; i++) {
        if (pred->states[i].page_id == page_id)
            return pred->states[i].error;
    }
    return 1.0; /* unknown = maximum error */
}

real_t predictor_confidence(const predictor_t *pred, page_id_t page_id)
{
    for (int i = 0; i < pred->num_states; i++) {
        if (pred->states[i].page_id == page_id)
            return pred->states[i].confidence;
    }
    return 0.0; /* unknown = zero confidence */
}
