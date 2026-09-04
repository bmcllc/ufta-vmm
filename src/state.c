/*
 * UFTA-VMM — state.c — State engine implementation
 *
 * σ = m · σ̂
 * H_p = h / sqrt(h² + r² + w² + 1)
 */

#include "ufta/state.h"
#include <math.h>
#include <string.h>

/* ── State update: compute magnitude and versor from raw ──────── */

void state_update(state_t *s)
{
    s->magnitude = vec3_norm(s->raw);
    s->versor    = vec3_normalize(s->raw);
}

/* ── Heat computation ─────────────────────────────────────────── */

heat_t state_heat(const state_t *s)
{
    heat_t h;

    /* H_p = h / sqrt(h² + r² + w² + 1) */
    real_t h_comp = fabs(s->raw.x);  /* heat component = x axis */
    real_t denom  = sqrt(s->raw.x * s->raw.x +
                         s->raw.y * s->raw.y +
                         s->raw.z * s->raw.z + 1.0);
    h.value = h_comp / denom;

    /* Clamp to [0, 1] */
    if (h.value < 0.0) h.value = 0.0;
    if (h.value > 1.0) h.value = 1.0;

    /* Classify */
    if (h.value > 0.8)
        h.level = HEAT_HOT;
    else if (h.value > 0.3)
        h.level = HEAT_WARM;
    else
        h.level = HEAT_COLD;

    return h;
}

/* ── Motion update: compute velocity from Δstate/Δt ──────────── */

void motion_update(motion_t *m, const state_t *prev, const state_t *curr, real_t dt)
{
    if (dt < 1e-15) {
        m->velocity = vec3_zero();
        m->speed    = 0.0;
        m->dir_predicted = curr->versor;
        return;
    }

    m->velocity = vec3_scale(vec3_sub(curr->raw, prev->raw), 1.0 / dt);
    m->speed    = vec3_norm(m->velocity);

    if (m->speed > 1e-12) {
        m->dir_predicted = vec3_normalize(m->velocity);
    } else {
        m->dir_predicted = curr->versor;
    }
}

/* ── Linear interpolation between states ──────────────────────── */

state_t state_lerp(const state_t *a, const state_t *b, real_t t)
{
    state_t result;
    result.raw = vec3_add(vec3_scale(a->raw, 1.0 - t), vec3_scale(b->raw, t));
    state_update(&result);
    return result;
}
