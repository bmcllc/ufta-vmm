/*
 * UFTA-VMM — Universal Field Theory Architecture — Virtual Memory Manager
 * types.h — Core type definitions
 *
 * σ = m · σ̂  (state = magnitude · versor)
 */

#ifndef UFTA_TYPES_H
#define UFTA_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

/* ── Scalar types ─────────────────────────────────────────────── */

typedef double   real_t;    /* primary real type     */
typedef uint64_t addr_t;    /* virtual address       */
typedef uint64_t page_id_t; /* unique page id        */
typedef uint32_t tier_id_t; /* tier index            */
typedef uint32_t chan_id_t;  /* channel index         */

/* ── Vector: 3-component spatial state ────────────────────────── */

typedef struct {
    real_t x, y, z;
} vec3_t;

static inline vec3_t vec3_zero(void) {
    return (vec3_t){0, 0, 0};
}

static inline vec3_t vec3_add(vec3_t a, vec3_t b) {
    return (vec3_t){a.x+b.x, a.y+b.y, a.z+b.z};
}

static inline vec3_t vec3_sub(vec3_t a, vec3_t b) {
    return (vec3_t){a.x-b.x, a.y-b.y, a.z-b.z};
}

static inline vec3_t vec3_scale(vec3_t v, real_t s) {
    return (vec3_t){v.x*s, v.y*s, v.z*s};
}

static inline real_t vec3_dot(vec3_t a, vec3_t b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

static inline real_t vec3_norm(vec3_t v) {
    return sqrt(vec3_dot(v, v));
}

static inline vec3_t vec3_normalize(vec3_t v) {
    real_t n = vec3_norm(v);
    if (n < 1e-12) return vec3_zero();
    return vec3_scale(v, 1.0 / n);
}

static inline real_t vec3_distance(vec3_t a, vec3_t b) {
    return vec3_norm(vec3_sub(a, b));
}

/* ── Rotation matrix (3×3) ────────────────────────────────────── */

typedef struct {
    real_t m[3][3];
} mat3_t;

static inline mat3_t mat3_identity(void) {
    mat3_t r = {{{0}}};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
    return r;
}

static inline vec3_t mat3_mul_vec(mat3_t M, vec3_t v) {
    return (vec3_t){
        M.m[0][0]*v.x + M.m[0][1]*v.y + M.m[0][2]*v.z,
        M.m[1][0]*v.x + M.m[1][1]*v.y + M.m[1][2]*v.z,
        M.m[2][0]*v.x + M.m[2][1]*v.y + M.m[2][2]*v.z
    };
}

static inline mat3_t mat3_mul(mat3_t A, mat3_t B) {
    mat3_t R = {{{0}}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++)
                R.m[i][j] += A.m[i][k] * B.m[k][j];
    return R;
}

/* ── Enumerations ─────────────────────────────────────────────── */

typedef enum {
    HEAT_COLD  = 0,   /* H ≤ 0.3  */
    HEAT_WARM  = 1,   /* 0.3 < H ≤ 0.8 */
    HEAT_HOT   = 2    /* H > 0.8  */
} heat_level_t;

typedef enum {
    PAGE_CLEAN  = 0,
  PAGE_DIRTY  = 1
} dirty_t;

typedef enum {
    MIGRATE_NONE     = 0,
    MIGRATE_UP       = 1,  /* to faster tier   */
    MIGRATE_DOWN     = 2,  /* to slower tier   */
    MIGRATE_EVICT    = 3,  /* to persistent    */
    MIGRATE_RESTORE  = 4   /* from persistent  */
} migrate_action_t;

typedef enum {
    OP_PHYSICS     = (1 << 0),
    OP_AI          = (1 << 1),
    OP_BEHAVIOR    = (1 << 2),
    OP_DIFFUSION   = (1 << 3),
    OP_CONTROL     = (1 << 4)
} operator_type_t;

/* ── Status codes ─────────────────────────────────────────────── */

typedef enum {
    UFTA_OK              =  0,
    UFTA_ERR_NOMEM       = -1,
    UFTA_ERR_NOTFOUND    = -2,
    UFTA_ERR_IO          = -3,
    UFTA_ERR_INVAL       = -4,
    UFTA_ERR_FULL        = -5,
    UFTA_ERR_BUSY        = -6,
    UFTA_ERR_PINNED      = -7,
    UFTA_ERR_BANDWIDTH   = -8
} ufta_err_t;

#endif /* UFTA_TYPES_H */
