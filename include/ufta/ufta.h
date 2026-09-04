/*
 * UFTA-VMM — ufta.h — Main public header (umbrella include)
 *
 * #include "ufta/ufta.h"  — pulls in everything
 */

#ifndef UFTA_H
#define UFTA_H

#include "types.h"
#include "state.h"
#include "tier.h"
#include "page.h"
#include "field.h"
#include "predictor.h"
#include "migrate.h"
#include "pipeline.h"
#include "backend.h"
#include "validate.h"

/* ── Version info ─────────────────────────────────────────────── */

#define UFTA_VERSION_MAJOR  1
#define UFTA_VERSION_MINOR  0
#define UFTA_VERSION_PATCH  0
#define UFTA_VERSION_STRING "1.0.0"

#endif /* UFTA_H */
