/*
 * UFTA-VMM — field.c — Field engine & operator implementation
 *
 * dσ/dt = O(σ, F) + P(σ, d̂) - C(σ)
 */

#include "ufta/field.h"
#include <math.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 * Operator implementations
 * ══════════════════════════════════════════════════════════════════ */

/* Physics: rigid-body-like rotation + force response */
real_t op_physics(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)sigma;
    /* Simple: force accelerates state in force direction */
    vec3_t ds = vec3_scale(force, dt);
    return vec3_norm(ds);
}

/* AI: learning-based adjustment (proportional to state magnitude) */
real_t op_ai(vec3_t sigma, vec3_t force, real_t dt)
{
    real_t mag = vec3_norm(sigma);
    if (mag < 1e-12) return 0.0;
    /* Apply force modulated by state magnitude */
    real_t response = vec3_dot(vec3_normalize(sigma), force) * mag * dt;
    return response;
}

/* Behavior: pattern-based nudging toward equilibrium */
real_t op_behavior(vec3_t sigma, vec3_t force, real_t dt)
{
    /* Tend toward unit sphere */
    real_t mag = vec3_norm(sigma);
    real_t drift = (1.0 - mag) * 0.1 * dt;
    return drift + vec3_dot(vec3_normalize(sigma), force) * 0.5 * dt;
}

/* Cosmic Operators */

/* Black Hole: convergent operator that pulls state toward origin */
real_t op_blackhole(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)force;
    /* Magnitude reduction proportional to current magnitude */
    real_t mag = vec3_norm(sigma);
    real_t ds = -0.2 * mag * dt; /* negative pulls inward */
    return ds;
}

/* White Hole: expansive operator that pushes state outward */
real_t op_whitehole(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)force;
    real_t mag = vec3_norm(sigma);
    real_t ds = 0.2 * mag * dt; /* positive pushes outward */
    return ds;
}

/* Vortex: rotational operator adding angular momentum */
real_t op_vortex(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)sigma;
    /* Use force magnitude to modulate rotation strength */
    real_t fmag = vec3_norm(force);
    real_t ds = 0.1 * fmag * dt; /* simple scalar representing rotation effect */
    return ds;
}

/* Entanglement: correlates sigma with force, returns a coupling term */
real_t op_entanglement(vec3_t sigma, vec3_t force, real_t dt)
{
    /* Simple model: dot product scaled by dt */
    real_t coupling = vec3_dot(sigma, force) * dt;
    return coupling;
}

/* Measurement: collapses state, returns magnitude scaled */
real_t op_measurement(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)force;
    real_t mag = vec3_norm(sigma);
    /* Simulate observation by returning a fraction of magnitude */
    return mag * 0.1 * dt;
}

/* Wave: propagating sinusoidal perturbation */
real_t op_wave(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)sigma;
    real_t fmag = vec3_norm(force);
    /* Sinusoidal modulation based on dt (as a proxy for time) */
    real_t ds = 0.05 * sin(dt) * fmag;
    return ds;
}

/* Diffusion: η · ∇²σ (simplified as damping) */
real_t op_diffusion(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)force;
    /* Simplified: diffusion spreads state magnitude toward neighbors */
    /* Here modeled as exponential decay toward mean */
    real_t mag = vec3_norm(sigma);
    real_t target = 0.5; /* mean field value */
    return (target - mag) * 0.01 * dt;
}

/* Reaction‑Diffusion: simple activator‑inhibitor model */
real_t op_reaction_diffusion(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)force;
    /* Use sigma magnitude as activator concentration u, and a fixed inhibitor v */
    real_t u = vec3_norm(sigma);
    real_t v = 0.3; /* constant inhibitor baseline */
    /* Parameters */
    const real_t Du = 0.01;   /* diffusion coefficient for u */
    const real_t Dv = 0.005;  /* diffusion coefficient for v */
    const real_t f = 0.04;    /* feed rate */
    const real_t k = 0.06;    /* kill rate */
    /* Gray‑Scott equations (simplified, no spatial laplacian) */
    real_t du = Du * ( -u * v * v + f * (1.0 - u) );
    real_t dv = Dv * (  u * v * v - (f + k) * v );
    (void)dv; /* suppress unused warning */
    /* Return net change in magnitude (activator) */
    return du * dt;
}

/* Control: PID-like stabilization */
real_t op_control(vec3_t sigma, vec3_t force, real_t dt)
{
    (void)force;
    /* Error from desired state (unit vector) */
    real_t mag = vec3_norm(sigma);
    real_t error = 1.0 - mag;
    /* Proportional control */
    return error * 2.0 * dt;
}

/* ══════════════════════════════════════════════════════════════════
 * Field Engine
 * ══════════════════════════════════════════════════════════════════ */

void field_engine_init(field_engine_t *fe)
{
    memset(fe, 0, sizeof(*fe));
    fe->diffusion_eta = 0.01;
    fe->control_gain  = 1.0;

    /* Register default operators */
    fe->operators[0] = operator_create(OP_PHYSICS,  1.0);
    fe->operators[1] = operator_create(OP_BEHAVIOR, 0.5);
    fe->operators[2] = operator_create(OP_CONTROL,  0.8);
    /* Register cosmic operators */
    fe->operators[3] = operator_create(OP_BLACKHOLE, 0.3);
    fe->operators[4] = operator_create(OP_WHITEHOLE, 0.3);
    fe->operators[5] = operator_create(OP_VORTEX,    0.2);
    fe->operators[6] = operator_create(OP_WAVE,      0.1);
    fe->operators[7] = operator_create(OP_REACTION_DIFFUSION, 0.05);
    fe->operators[8] = operator_create(OP_ENTANGLEMENT, 0.15);
    fe->operators[9] = operator_create(OP_MEASUREMENT, 0.05);
    fe->num_operators = 10;

    /* Add a default ambient field */
    fe->fields[0] = field_create(vec3_zero(), 0.1, 0.01);
    fe->num_fields = 1;
}

int field_engine_add_field(field_engine_t *fe, const field_t *f)
{
    if (fe->num_fields >= UFTA_MAX_FIELDS) return UFTA_ERR_FULL;
    fe->fields[fe->num_fields++] = *f;
    return UFTA_OK;
}

int field_engine_add_operator(field_engine_t *fe, const operator_t *op)
{
    if (fe->num_operators >= UFTA_MAX_OPERATORS) return UFTA_ERR_FULL;
    fe->operators[fe->num_operators++] = *op;
    return UFTA_OK;
}

/* ── Apply all operators to a state ───────────────────────────── */

vec3_t field_engine_apply(const field_engine_t *fe, const state_t *s, real_t dt)
{
    vec3_t total_ds = vec3_zero();

    for (int i = 0; i < fe->num_operators; i++) {
        const operator_t *op = &fe->operators[i];
        for (int f = 0; f < fe->num_fields; f++) {
            real_t ds_mag = op->apply(s->raw, fe->fields[f].force, dt);
            ds_mag *= op->strength * fe->fields[f].strength;
            /* Direction: along versor */
            vec3_t ds = vec3_scale(s->versor, ds_mag);
            total_ds = vec3_add(total_ds, ds);
        }
    }

    /* Apply diffusion damping */
    if (fe->diffusion_eta > 0) {
        real_t decay = op_diffusion(s->raw, vec3_zero(), dt);
        total_ds = vec3_add(total_ds, vec3_scale(s->versor, decay));
    }

    return total_ds;
}

/* ── Step a single page through the field engine ──────────────── */

void field_engine_step_page(const field_engine_t *fe, page_t *p, real_t dt)
{
    vec3_t ds = field_engine_apply(fe, &p->state, dt);
    p->state.raw = vec3_add(p->state.raw, ds);
    state_update(&p->state);
    page_update_heat(p);
}

/* ── Factory functions ────────────────────────────────────────── */

field_t field_create(vec3_t origin, real_t strength, real_t decay)
{
    field_t f;
    f.force    = origin;
    f.strength = strength;
    f.decay    = decay;
    return f;
}

operator_t operator_create(operator_type_t type, real_t strength)
{
    operator_t op;
    memset(&op, 0, sizeof(op));
    op.type     = type;
    op.strength = strength;
    op.matrix   = mat3_identity();

    switch (type) {
        case OP_PHYSICS:   op.apply = op_physics;   break;
        case OP_AI:        op.apply = op_ai;         break;
        case OP_BEHAVIOR:  op.apply = op_behavior;   break;
        case OP_DIFFUSION: op.apply = op_diffusion;  break;
        case OP_CONTROL:   op.apply = op_control;    break;
        case OP_BLACKHOLE: op.apply = op_blackhole; break;
        case OP_WHITEHOLE: op.apply = op_whitehole; break;
        case OP_VORTEX:    op.apply = op_vortex;    break;
        case OP_WAVE:      op.apply = op_wave;      break;
        case OP_ENTANGLEMENT: op.apply = op_entanglement; break;
        case OP_MEASUREMENT:  op.apply = op_measurement;  break;
        default:           op.apply = op_physics;    break;
    }
    /* No longer need heuristic mapping based on strength */
    return op;
}
