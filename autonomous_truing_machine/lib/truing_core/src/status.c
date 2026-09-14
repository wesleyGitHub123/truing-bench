#include "truing/status.h"

#include <stddef.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const char *const k_status_str[TRUING_STATUS__COUNT] = {
    "unset", "valid", "suspect", "rejected", "unavailable", "stale",
};

static const char *const k_reason_str[TRUING_REASON__COUNT] = {
    "NONE",
    "NO_ONSET_DETECTED",
    "ONSET_COUNT_MISMATCH",
    "LOW_SNR",
    "AMBIGUOUS_PEAK",
    "FREQ_OUT_OF_RANGE",
    "NO_F2_PARTNER",
    "PROVISIONAL_MODE_ID",
    "MODEL_REJECTED",
    "SENSOR_TIMEOUT",
    "VALUE_OUT_OF_RANGE",
    "NOT_IMPLEMENTED",
    "CALIBRATION_MISSING",
    "STALE_MEASUREMENT",
    "PARTIAL_WHEEL_STATE",
    "ARTIFACT_INVALID",
    "STALE_INTENT",
    "REQUIRES_ARTIFACT_REGENERATION",
    "MEAN_TENSION_MODEL_UNAVAILABLE",
    "TENSION_NOT_VERIFICATION_GRADE",
    "CANCELLED",
    "WHEEL_REFERENCE_LOST",
    "CAPTURE_OVERRUN",
    "EXCITATION_UNAVAILABLE",
};

static const char *const k_terminal_str[TRUING_TERMINAL__COUNT] = {
    "NONE",
    "CONVERGED",
    "CONVERGED_GEOMETRIC_ONLY",
    "ABORT_NO_PROGRESS",
    "ABORT_MAX_CYCLES",
    "ABORT_UNSAFE_ADJUSTMENT",
    "ABORT_NO_MODEL",
    "ABORT_OPERATOR",
    "ABORT_PARTIAL_STATE",
};

static const char *const k_source_str[TRUING_SOURCE__COUNT] = {
    "unset", "real", "host_service", "recorded", "synthetic",
};

static const char *const k_check_str[TRUING_CHECK__COUNT] = {
    "OK",
    "ERR_NULL",
    "ERR_STATUS_UNSET",
    "ERR_STATUS_RANGE",
    "ERR_REASON_RANGE",
    "ERR_REASON_REQUIRED",
    "ERR_REASON_ON_VALID",
    "ERR_SOURCE_UNSET",
    "ERR_NOT_FINITE",
    "ERR_NOT_POSITIVE",
    "ERR_MODE_IDENTITY_UNSET",
    "ERR_VALID_WITHOUT_CONFIRMED_MODE",
    "ERR_TOO_MANY_CANDIDATES",
    "ERR_MODEL_UNSET",
    "ERR_ANGLE_RANGE",
    "ERR_EMBEDDED_RECORD",
};

_Static_assert(ARRAY_LEN(k_status_str) == TRUING_STATUS__COUNT, "status table");
_Static_assert(ARRAY_LEN(k_reason_str) == TRUING_REASON__COUNT, "reason table");
_Static_assert(ARRAY_LEN(k_terminal_str) == TRUING_TERMINAL__COUNT, "terminal table");
_Static_assert(ARRAY_LEN(k_source_str) == TRUING_SOURCE__COUNT, "source table");
_Static_assert(ARRAY_LEN(k_check_str) == TRUING_CHECK__COUNT, "check table");

truing_check_t truing_record_meta_check(const truing_record_meta_t *meta)
{
    if (meta == NULL) {
        return TRUING_CHECK_ERR_NULL;
    }
    if (meta->status == TRUING_STATUS_UNSET) {
        return TRUING_CHECK_ERR_STATUS_UNSET;
    }
    if ((unsigned)meta->status >= TRUING_STATUS__COUNT) {
        return TRUING_CHECK_ERR_STATUS_RANGE;
    }
    if ((unsigned)meta->reason_code >= TRUING_REASON__COUNT) {
        return TRUING_CHECK_ERR_REASON_RANGE;
    }
    if (meta->status == TRUING_STATUS_VALID && meta->reason_code != TRUING_REASON_NONE) {
        return TRUING_CHECK_ERR_REASON_ON_VALID;
    }
    if (meta->status != TRUING_STATUS_VALID && meta->reason_code == TRUING_REASON_NONE) {
        return TRUING_CHECK_ERR_REASON_REQUIRED;
    }
    if (meta->source_impl == TRUING_SOURCE_UNSET || (unsigned)meta->source_impl >= TRUING_SOURCE__COUNT) {
        return TRUING_CHECK_ERR_SOURCE_UNSET;
    }
    return TRUING_CHECK_OK;
}

bool truing_status_is_solver_admissible(truing_status_t s)
{
    return s == TRUING_STATUS_VALID || s == TRUING_STATUS_SUSPECT;
}

bool truing_status_is_verification_grade(truing_status_t s)
{
    return s == TRUING_STATUS_VALID;
}

bool truing_terminal_is_success(truing_terminal_result_t t)
{
    return t == TRUING_TERMINAL_CONVERGED;
}

bool truing_terminal_is_abort(truing_terminal_result_t t)
{
    switch (t) {
    case TRUING_TERMINAL_ABORT_NO_PROGRESS:
    case TRUING_TERMINAL_ABORT_MAX_CYCLES:
    case TRUING_TERMINAL_ABORT_UNSAFE_ADJUSTMENT:
    case TRUING_TERMINAL_ABORT_NO_MODEL:
    case TRUING_TERMINAL_ABORT_OPERATOR:
    case TRUING_TERMINAL_ABORT_PARTIAL_STATE:
        return true;
    default:
        return false;
    }
}

const char *truing_status_str(truing_status_t s)
{
    return (unsigned)s < TRUING_STATUS__COUNT ? k_status_str[s] : "?";
}

const char *truing_reason_str(truing_reason_t r)
{
    return (unsigned)r < TRUING_REASON__COUNT ? k_reason_str[r] : "?";
}

const char *truing_terminal_str(truing_terminal_result_t t)
{
    return (unsigned)t < TRUING_TERMINAL__COUNT ? k_terminal_str[t] : "?";
}

const char *truing_source_impl_str(truing_source_impl_t s)
{
    return (unsigned)s < TRUING_SOURCE__COUNT ? k_source_str[s] : "?";
}

const char *truing_check_str(truing_check_t c)
{
    return (unsigned)c < TRUING_CHECK__COUNT ? k_check_str[c] : "?";
}
