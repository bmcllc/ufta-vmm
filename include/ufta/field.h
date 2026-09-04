/*
 * UFTA-VMM — field.h — Field engine & operators
 *
 * dσ/dt = O(σ, F) + P(σ, d̂) - C(σ)
 */

#ifndef UFTA_FIELD_H
#define UFTA_FIELD_H

#include "types.h"
#include "state.h"
#include "page.h"

/* ── Field: external influence on state ───────────────────────── */

typedef struct {
    vec3_t  force;       /* F: ambient field force     */
    real_t  strength;    /* field intensity            */
    real_t  decay;       /* field decay rate           */
} field_t;

/* ── Operator (transformation on state) ──────────────────────── */

typedef struct {
    operator_type_t type;
    real_t          strength;
    mat3_t          matrix;      /* transformation matrix */
    real_t          (*apply)(vec3_t sigma, vec3_t force, real_t dt);
} operator_t;

/* ── Field Engine: manages fields and operators ──────────────── */

#define UFTA_MAX_FIELDS    16
#define UFTA_MAX_OPERATORS 16

typedef struct {
    field_t    fields[UFTA_MAX_FIELDS];
    int        num_fields;

    operator_t operators[UFTA_MAX_OPERATORS];
    int        num_operators;

    /* Master equation coefficients */
    real_t     diffusion_eta;   /* η: diffusion coefficient */
    real_t     control_gain;    /* correction gain          */
} field_engine_t;

/* ── Operator functions (implementations) ────────────────────── */

/* Physics operator: rigid-body-like state evolution */
real_t op_physics(vec3_t sigma, vec3_t force, real_t dt);

/* AI operator: learning-based state adjustment */
real_t op_ai(vec3_t sigma, vec3_t force, real_t dt);

/* Behavior operator: pattern-based state nudging */
real_t op_behavior(vec3_t sigma, vec3_t force, real_t dt);

/* Diffusion operator: η · ∇²σ (spatial spread) */
real_t op_diffusion(vec3_t sigma, vec3_t force, real_t dt);

/* Control operator: stabilization / correction */
real_t op_control(vec3_t sigma, vec3_t force, real_t dt);

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize field engine with defaults */
void field_engine_init(field_engine_t *fe);

/* Add a field to the engine */
int field_engine_add_field(field_engine_t *fe, const field_t *f);

/* Add an operator to the engine */
int field_engine_add_operator(field_engine_t *fe, const operator_t *op);

/* Apply all operators to a state: dσ/dt = Σ O_i(σ, F) - C(σ) */
vec3_t field_engine_apply(const field_engine_t *fe, const state_t *s, real_t dt);

/* Apply master equation to a page */
void field_engine_step_page(const field_engine_t *fe, page_t *p, real_t dt);

/* Create a field at a point with given strength */
field_t field_create(vec3_t origin, real_t strength, real_t decay);

/* Create an operator of given type */
operator_t operator_create(operator_type_t type, real_t strength);

#endif /* UFTA_FIELD_H */
