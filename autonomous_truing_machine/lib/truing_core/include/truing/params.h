/**
 * @file params.h
 * Runtime-addressable configuration parameters, their mutability class, and
 * SET_PARAMETER admissibility as a function of state AND class (SPEC §12.3.1).
 *
 * Every runtime-selectable field belongs to exactly one class. A field this table
 * cannot classify is treated as ARTIFACT_BOUND (the conservative default) and is
 * reported as a specification bug by truing_param_is_classified().
 *
 * Host-only model-preparation values (n_fit_samples_*, T_target_assumed, ...) are
 * deliberately NOT addressable here (SPEC §12.3.1 "Host-only values are outside
 * the command surface").
 */
#ifndef TRUING_PARAMS_H
#define TRUING_PARAMS_H

#include <stdbool.h>

#include "truing/state_ids.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_PARAM_UNSET = 0,
    /* session-mutable (cycle control only) */
    TRUING_PARAM_MAX_CYCLES,
    TRUING_PARAM_MEASUREMENT_RETRY_COUNT,
    TRUING_PARAM_EXCITATION_SETTLE_MS,
    TRUING_PARAM_MIN_RELATIVE_IMPROVEMENT,
    TRUING_PARAM_CONSECUTIVE_NON_IMPROVING_CYCLES,
    TRUING_PARAM_PARTIAL_STATE_REMEASURE_ATTEMPTS,
    /* session-fixed */
    TRUING_PARAM_TENSION_MODEL_PROFILE_ID,
    TRUING_PARAM_TARGET_TENSION,
    TRUING_PARAM_ADJUSTMENT_DEADBAND,
    TRUING_PARAM_LATERAL_DEADBAND,
    TRUING_PARAM_MAX_ADJUSTMENT_REVOLUTIONS,
    /* artifact-bound firmware configuration / binding */
    TRUING_PARAM_N_SPOKES,
    TRUING_PARAM_N_CROSS,
    TRUING_PARAM_N_RIM_ANGLES,
    TRUING_PARAM_N_LAT,
    TRUING_PARAM_N_RAD,
    TRUING_PARAM_TOL_LATERAL,
    TRUING_PARAM_TOL_RADIAL,
    TRUING_PARAM_TOL_TENSION,
    TRUING_PARAM_TOL_TENSION_CV,
    TRUING_PARAM_TOL_MEAN_TENSION_ERROR,
    TRUING_PARAM_TOL_ANGULAR,
    TRUING_PARAM_MAX_CONDITION_NUMBER,
    TRUING_PARAM_RANK_TOLERANCE,
    TRUING_PARAM_N_MT_DISPLACEMENT_TOLERANCE,
    TRUING_PARAM_N_MT_TENSION_TOLERANCE,
    TRUING_PARAM_N_MT_UNIQUENESS_THRESHOLD,
    TRUING_PARAM_SYMMETRY_ANGLE_TOLERANCE,
    TRUING_PARAM_TRUST_RADIAL,
    TRUING_PARAM_TRUST_TENSION,
    TRUING_PARAM_HUB_GEOMETRY,
    TRUING_PARAM_ASYMMETRIC,
    TRUING_PARAM_SPOKE_GEOMETRY,          /* spoke diameter, material modulus */
    TRUING_PARAM_INDEXING_ORIGIN,
    TRUING_PARAM_RIM_DIAMETER,
    TRUING_PARAM_C_SIDE_A,
    TRUING_PARAM_C_SIDE_B,
    TRUING_PARAM_EXPECTED_INFLUENCE_FINGERPRINT,
    TRUING_PARAM_COMPATIBLE_MODEL_ASSUMPTIONS,
    TRUING_PARAM__COUNT
} truing_param_id_t;

typedef enum {
    TRUING_PARAM_CLASS_UNCLASSIFIED = 0,  /* treated as artifact-bound; a spec bug if reached */
    TRUING_PARAM_CLASS_SESSION_MUTABLE,
    TRUING_PARAM_CLASS_SESSION_FIXED,
    TRUING_PARAM_CLASS_ARTIFACT_BOUND,
} truing_param_class_t;

typedef enum {
    TRUING_SET_PARAM_ACCEPT = 0,
    TRUING_SET_PARAM_REJECT_REQUIRES_ARTIFACT_REGENERATION,  /* artifact-bound: always refused */
    TRUING_SET_PARAM_REJECT_SESSION_IN_PROGRESS,             /* session-fixed outside READY */
    TRUING_SET_PARAM_REJECT_AUTONOMOUS_STATE,                /* nothing is settable mid-operation */
    TRUING_SET_PARAM_REJECT_STATE_INACTIVE,                  /* BOOT / INITIALIZE / TERMINAL */
    TRUING_SET_PARAM_REJECT_UNKNOWN_PARAM,
} truing_set_param_verdict_t;

truing_param_class_t truing_param_class(truing_param_id_t id);
bool truing_param_is_classified(truing_param_id_t id);
truing_set_param_verdict_t truing_set_parameter_admissible(truing_state_t state, truing_param_id_t id);
/* Reason code to report for a verdict (NONE when none is defined by SPEC §13.2). */
truing_reason_t truing_set_param_verdict_reason(truing_set_param_verdict_t v);

const char *truing_param_str(truing_param_id_t id);
const char *truing_param_class_str(truing_param_class_t c);
const char *truing_set_param_verdict_str(truing_set_param_verdict_t v);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_PARAMS_H */
