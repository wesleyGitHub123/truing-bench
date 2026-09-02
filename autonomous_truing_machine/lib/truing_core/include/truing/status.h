/**
 * @file status.h
 * Status, reason-code, terminal-result and provenance enumerations (SPEC §6.2, §13.2)
 * plus the per-record universal fields and their validity check.
 *
 * SPEC §13.2: terminal/session result codes and measurement-level reason codes are
 * SEPARATE types. They are never merged here.
 *
 * SPEC §6.2: `status` is not optional and has no default. In C a zero-initialised
 * record is unavoidable, so the zero value of every enum below is an explicit
 * UNSET sentinel that validation REJECTS. A record that was never assigned a
 * status is therefore invalid, not silently "valid".
 */
#ifndef TRUING_STATUS_H
#define TRUING_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_STATUS_UNSET = 0,   /* never legal in a stored record */
    TRUING_STATUS_VALID,
    TRUING_STATUS_SUSPECT,
    TRUING_STATUS_REJECTED,
    TRUING_STATUS_UNAVAILABLE,
    TRUING_STATUS_STALE,
    TRUING_STATUS__COUNT
} truing_status_t;

/* SPEC §13.2 minimum reason-code set, verbatim, plus NONE for valid records. */
typedef enum {
    TRUING_REASON_NONE = 0,
    TRUING_REASON_NO_ONSET_DETECTED,
    TRUING_REASON_ONSET_COUNT_MISMATCH,
    TRUING_REASON_LOW_SNR,
    TRUING_REASON_AMBIGUOUS_PEAK,
    TRUING_REASON_FREQ_OUT_OF_RANGE,
    TRUING_REASON_NO_F2_PARTNER,
    TRUING_REASON_PROVISIONAL_MODE_ID,
    TRUING_REASON_MODEL_REJECTED,
    TRUING_REASON_SENSOR_TIMEOUT,
    TRUING_REASON_VALUE_OUT_OF_RANGE,
    TRUING_REASON_NOT_IMPLEMENTED,
    TRUING_REASON_CALIBRATION_MISSING,
    TRUING_REASON_STALE_MEASUREMENT,
    TRUING_REASON_PARTIAL_WHEEL_STATE,
    TRUING_REASON_ARTIFACT_INVALID,
    TRUING_REASON_STALE_INTENT,
    TRUING_REASON_REQUIRES_ARTIFACT_REGENERATION,
    TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE,
    TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE,
    /* Extensions beyond the SPEC §13.2 minimum set (recorded in docs/IMPLEMENTATION_NOTES.md):
     * CANCELLED — a measurement discarded because ABORT cancelled it at the acoustic
     *             boundary (SPEC §7.4, §12.3);
     * WHEEL_REFERENCE_LOST — the navigation subsystem can no longer vouch for the wheel's
     *             logical position (slip, fault, or no reference established) (SPEC §10A). */
    TRUING_REASON_CANCELLED,
    TRUING_REASON_WHEEL_REFERENCE_LOST,
    /* CAPTURE_OVERRUN — the audio front end dropped samples during a capture; the spectrum of a
     *             discontinuous record is corrupt, so the measurement is rejected (SPEC §9.4). */
    TRUING_REASON_CAPTURE_OVERRUN,
    TRUING_REASON__COUNT
} truing_reason_t;

/* SPEC §13.2 / §8.6.2 terminal result vocabulary — the ONLY one. */
typedef enum {
    TRUING_TERMINAL_NONE = 0,  /* session not finished */
    TRUING_TERMINAL_CONVERGED,
    TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY,
    TRUING_TERMINAL_ABORT_NO_PROGRESS,
    TRUING_TERMINAL_ABORT_MAX_CYCLES,
    TRUING_TERMINAL_ABORT_UNSAFE_ADJUSTMENT,
    TRUING_TERMINAL_ABORT_NO_MODEL,
    TRUING_TERMINAL_ABORT_OPERATOR,
    TRUING_TERMINAL_ABORT_PARTIAL_STATE,
    TRUING_TERMINAL__COUNT
} truing_terminal_result_t;

/* SPEC §6.2: source_impl is an enum, NOT a string. */
typedef enum {
    TRUING_SOURCE_UNSET = 0,
    TRUING_SOURCE_REAL,
    TRUING_SOURCE_HOST_SERVICE,
    TRUING_SOURCE_RECORDED,
    TRUING_SOURCE_SYNTHETIC,
    TRUING_SOURCE__COUNT
} truing_source_impl_t;

/* Per-record universal fields (SPEC §6.2). Session-constant provenance lives in the
 * session header (truing/session.h), never duplicated here. */
typedef struct {
    truing_status_t      status;
    truing_reason_t      reason_code;   /* required when status != valid */
    uint8_t              cycle_index;
    uint32_t             timestamp_ms;  /* ms since boot (SPEC §6.6) */
    truing_source_impl_t source_impl;
} truing_record_meta_t;

/* Result of any structural validity check in the core. */
typedef enum {
    TRUING_CHECK_OK = 0,
    TRUING_CHECK_ERR_NULL,
    TRUING_CHECK_ERR_STATUS_UNSET,
    TRUING_CHECK_ERR_STATUS_RANGE,
    TRUING_CHECK_ERR_REASON_RANGE,
    TRUING_CHECK_ERR_REASON_REQUIRED,        /* status != valid but reason == NONE */
    TRUING_CHECK_ERR_REASON_ON_VALID,        /* status == valid but reason != NONE */
    TRUING_CHECK_ERR_SOURCE_UNSET,
    TRUING_CHECK_ERR_NOT_FINITE,
    TRUING_CHECK_ERR_NOT_POSITIVE,
    TRUING_CHECK_ERR_MODE_IDENTITY_UNSET,
    TRUING_CHECK_ERR_VALID_WITHOUT_CONFIRMED_MODE, /* SPEC §4.4.1 / §6.3 */
    TRUING_CHECK_ERR_TOO_MANY_CANDIDATES,
    TRUING_CHECK_ERR_MODEL_UNSET,
    TRUING_CHECK_ERR_ANGLE_RANGE,
    TRUING_CHECK_ERR_EMBEDDED_RECORD,
    TRUING_CHECK__COUNT
} truing_check_t;

truing_check_t truing_record_meta_check(const truing_record_meta_t *meta);

/* SPEC §8.11: solver-admissible = valid OR suspect. */
bool truing_status_is_solver_admissible(truing_status_t s);
/* SPEC §8.6.3: verification-grade = valid ONLY. */
bool truing_status_is_verification_grade(truing_status_t s);
/* SPEC §8.6.2: only CONVERGED may be presented as successful truing. */
bool truing_terminal_is_success(truing_terminal_result_t t);
bool truing_terminal_is_abort(truing_terminal_result_t t);

/* String tables for telemetry/logging. Never NULL for in-range values; "?" otherwise. */
const char *truing_status_str(truing_status_t s);
const char *truing_reason_str(truing_reason_t r);
const char *truing_terminal_str(truing_terminal_result_t t);
const char *truing_source_impl_str(truing_source_impl_t s);
const char *truing_check_str(truing_check_t c);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_STATUS_H */
