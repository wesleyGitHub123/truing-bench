#include "truing/params.h"

#include <math.h>
#include <stddef.h>

#include "truing/config.h"

typedef struct {
    const char          *name;
    truing_param_class_t cls;
} param_entry_t;

/* SPEC §12.3.1 classification table. */
static const param_entry_t k_params[TRUING_PARAM__COUNT] = {
    { "UNSET",                          TRUING_PARAM_CLASS_UNCLASSIFIED },
    /* session-mutable (cycle control only) */
    { "max_cycles",                     TRUING_PARAM_CLASS_SESSION_MUTABLE },
    { "measurement_retry_count",        TRUING_PARAM_CLASS_SESSION_MUTABLE },
    { "excitation_settle_ms",           TRUING_PARAM_CLASS_SESSION_MUTABLE },
    { "min_relative_improvement",       TRUING_PARAM_CLASS_SESSION_MUTABLE },
    { "consecutive_non_improving_cycles", TRUING_PARAM_CLASS_SESSION_MUTABLE },
    { "partial_state_remeasure_attempts", TRUING_PARAM_CLASS_SESSION_MUTABLE },
    /* session-fixed */
    { "tension_model_profile_id",       TRUING_PARAM_CLASS_SESSION_FIXED },
    { "target_tension",                 TRUING_PARAM_CLASS_SESSION_FIXED },
    { "adjustment_deadband",            TRUING_PARAM_CLASS_SESSION_FIXED },
    { "lateral_deadband",               TRUING_PARAM_CLASS_SESSION_FIXED },
    { "max_adjustment_revolutions",     TRUING_PARAM_CLASS_SESSION_FIXED },
    /* artifact-bound */
    { "n_spokes",                       TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "n_cross",                        TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "n_rim_angles",                   TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "N_lat",                          TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "N_rad",                          TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "tol_lateral",                    TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "tol_radial",                     TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "tol_tension",                    TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "tol_tension_cv",                 TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "tol_mean_tension_error",         TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "tol_angular",                    TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "max_condition_number",           TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "rank_tolerance",                 TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "n_mt_displacement_tolerance",    TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "n_mt_tension_tolerance",         TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "n_mt_uniqueness_threshold",      TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "symmetry_angle_tolerance",       TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "trust_radial",                   TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "trust_tension",                  TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "hub_geometry",                   TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "asymmetric",                     TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "spoke_geometry",                 TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "indexing_origin",                TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "rim_diameter",                   TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "c_side_a",                       TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "c_side_b",                       TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "expected_influence_fingerprint", TRUING_PARAM_CLASS_ARTIFACT_BOUND },
    { "compatible_model_assumptions",   TRUING_PARAM_CLASS_ARTIFACT_BOUND },
};

static const char *const k_class_str[] = { "unclassified", "session_mutable", "session_fixed", "artifact_bound" };
static const char *const k_verdict_str[] = {
    "ACCEPT",
    "REJECT_REQUIRES_ARTIFACT_REGENERATION",
    "REJECT_SESSION_IN_PROGRESS",
    "REJECT_AUTONOMOUS_STATE",
    "REJECT_STATE_INACTIVE",
    "REJECT_UNKNOWN_PARAM",
};

truing_param_class_t truing_param_class(truing_param_id_t id)
{
    if (id == TRUING_PARAM_UNSET || (unsigned)id >= TRUING_PARAM__COUNT) {
        return TRUING_PARAM_CLASS_UNCLASSIFIED;
    }
    return k_params[id].cls;
}

bool truing_param_is_classified(truing_param_id_t id)
{
    return truing_param_class(id) != TRUING_PARAM_CLASS_UNCLASSIFIED;
}

truing_set_param_verdict_t truing_set_parameter_admissible(truing_state_t state, truing_param_id_t id)
{
    if (id == TRUING_PARAM_UNSET || (unsigned)id >= TRUING_PARAM__COUNT) {
        return TRUING_SET_PARAM_REJECT_UNKNOWN_PARAM;
    }
    truing_param_class_t cls = truing_param_class(id);
    if (cls == TRUING_PARAM_CLASS_UNCLASSIFIED) {
        cls = TRUING_PARAM_CLASS_ARTIFACT_BOUND;   /* conservative default (SPEC §12.3.1) */
    }
    /* Artifact-bound: refused in EVERY state. Never applied with the old artifact loaded. */
    if (cls == TRUING_PARAM_CLASS_ARTIFACT_BOUND) {
        return TRUING_SET_PARAM_REJECT_REQUIRES_ARTIFACT_REGENERATION;
    }
    switch (truing_state_class(state)) {
    case TRUING_STATE_CLASS_READY:
        return TRUING_SET_PARAM_ACCEPT;   /* session-mutable AND session-fixed */
    case TRUING_STATE_CLASS_WAIT_FOR_OPERATOR:
        return cls == TRUING_PARAM_CLASS_SESSION_MUTABLE ? TRUING_SET_PARAM_ACCEPT
                                                          : TRUING_SET_PARAM_REJECT_SESSION_IN_PROGRESS;
    case TRUING_STATE_CLASS_AUTONOMOUS:
        return TRUING_SET_PARAM_REJECT_AUTONOMOUS_STATE;
    case TRUING_STATE_CLASS_INACTIVE:
    default:
        return TRUING_SET_PARAM_REJECT_STATE_INACTIVE;
    }
}

static bool fits_u8(float v)
{
    return isfinite(v) && v >= 0.0f && v <= 255.0f && v == floorf(v);
}

static bool fits_u16(float v)
{
    return isfinite(v) && v >= 0.0f && v <= 65535.0f && v == floorf(v);
}

static bool fits_u32(float v)
{
    return isfinite(v) && v >= 0.0f && v <= 4294967040.0f && v == floorf(v);
}

bool truing_param_apply(struct truing_solver_config *cfg, truing_param_id_t id, float value)
{
    if (cfg == NULL) {
        return false;
    }
    const truing_param_class_t cls = truing_param_class(id);
    if (cls != TRUING_PARAM_CLASS_SESSION_MUTABLE && cls != TRUING_PARAM_CLASS_SESSION_FIXED) {
        return false;   /* artifact-bound and unknown ids are never applied (SPEC §12.3.1) */
    }
    switch (id) {
    case TRUING_PARAM_MAX_CYCLES:
        if (!fits_u8(value)) return false;
        cfg->max_cycles = (uint8_t)value;
        return true;
    case TRUING_PARAM_MEASUREMENT_RETRY_COUNT:
        if (!fits_u8(value)) return false;
        cfg->measurement_retry_count = (uint8_t)value;
        return true;
    case TRUING_PARAM_EXCITATION_SETTLE_MS:
        if (!fits_u16(value)) return false;
        cfg->excitation_settle_ms = (uint16_t)value;
        return true;
    case TRUING_PARAM_MIN_RELATIVE_IMPROVEMENT:
        if (!isfinite(value)) return false;
        cfg->min_relative_improvement = value;
        return true;
    case TRUING_PARAM_CONSECUTIVE_NON_IMPROVING_CYCLES:
        if (!fits_u8(value)) return false;
        cfg->consecutive_non_improving_cycles = (uint8_t)value;
        return true;
    case TRUING_PARAM_PARTIAL_STATE_REMEASURE_ATTEMPTS:
        if (!fits_u8(value)) return false;
        cfg->partial_state_remeasure_attempts = (uint8_t)value;
        return true;
    case TRUING_PARAM_TENSION_MODEL_PROFILE_ID:
        if (!fits_u32(value)) return false;
        cfg->tension_model_profile_id = (uint32_t)value;
        return true;
    case TRUING_PARAM_TARGET_TENSION:
        if (!isfinite(value)) return false;
        cfg->target_tension_n = value;
        return true;
    case TRUING_PARAM_ADJUSTMENT_DEADBAND:
        if (!isfinite(value)) return false;
        cfg->adjustment_deadband_rev = value;
        return true;
    case TRUING_PARAM_LATERAL_DEADBAND:
        if (!isfinite(value)) return false;
        cfg->lateral_deadband_mm = value;
        return true;
    case TRUING_PARAM_MAX_ADJUSTMENT_REVOLUTIONS:
        if (!isfinite(value)) return false;
        cfg->max_adjustment_revolutions = value;
        return true;
    default:
        return false;
    }
}

truing_reason_t truing_set_param_verdict_reason(truing_set_param_verdict_t v)
{
    return v == TRUING_SET_PARAM_REJECT_REQUIRES_ARTIFACT_REGENERATION ? TRUING_REASON_REQUIRES_ARTIFACT_REGENERATION
                                                                       : TRUING_REASON_NONE;
}

const char *truing_param_str(truing_param_id_t id)
{
    return (unsigned)id < TRUING_PARAM__COUNT ? k_params[id].name : "?";
}

const char *truing_param_class_str(truing_param_class_t c)
{
    return (unsigned)c < sizeof(k_class_str) / sizeof(k_class_str[0]) ? k_class_str[c] : "?";
}

const char *truing_set_param_verdict_str(truing_set_param_verdict_t v)
{
    return (unsigned)v < sizeof(k_verdict_str) / sizeof(k_verdict_str[0]) ? k_verdict_str[v] : "?";
}
