/**
 * @file acoustic_if.h
 * Acoustic subsystem external interface (SPEC §9.1): ONE coarse call.
 *
 *   measure_spoke_tension(spoke_id, wheel_geometry) -> TensionEstimate
 *
 * Excitation, capture, analysis, damping ritual, retry (SPEC §7.4) and model
 * application are INTERNAL to the implementation and invisible here. This is a
 * sensor+actuator composite, not a passive sensor. Cancellation (ABORT) is
 * delivered as a cooperative request; the implementation returns at its next
 * internal safe point.
 *
 * reset_session() clears any latched request state (a cancel that no measurement
 * consumed, attempt bookkeeping) so a stale one-shot cannot cross a session
 * boundary. It does NOT re-open the front end or touch capture evidence. The
 * orchestrator calls it from begin_session(); an implementation with nothing
 * latched leaves it empty.
 */
#ifndef TRUING_HAL_ACOUSTIC_IF_H
#define TRUING_HAL_ACOUSTIC_IF_H

#include <stdint.h>

#include "truing/config.h"
#include "truing/measurements.h"
#include "truing/status.h"
#include "truing_hal/clock_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_acoustic_if truing_acoustic_if_t;

struct truing_acoustic_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    void (*measure_spoke_tension)(truing_acoustic_if_t *self, uint8_t spoke_id,
                                  const truing_wheel_class_config_t *wheel_geometry,
                                  uint8_t cycle_index, truing_tension_estimate_t *out);
    void (*request_cancel)(truing_acoustic_if_t *self);
    /* Clear latched request state at a session boundary (may be NULL: nothing to clear). */
    void (*reset_session)(truing_acoustic_if_t *self);
    /* Can this subsystem excite and measure every spoke a session will ask for? Asked at session
     * admission; false fills `reason`. May be NULL: nothing to be ready about (synthetic, stub). */
    bool (*ready)(truing_acoustic_if_t *self, truing_reason_t *reason);
    void *ctx;
};

/* Null-safe wrappers. A missing implementation yields `unavailable` / NOT_IMPLEMENTED (P2). */
void truing_acoustic_measure(truing_acoustic_if_t *self, uint8_t spoke_id,
                             const truing_wheel_class_config_t *wheel_geometry,
                             uint8_t cycle_index, truing_tension_estimate_t *out);
void truing_acoustic_request_cancel(truing_acoustic_if_t *self);
/* Discard any cancel that no measurement consumed, plus attempt bookkeeping, so it cannot
 * leak into the next session. No-op when the interface or its hook is absent. */
void truing_acoustic_reset_session(truing_acoustic_if_t *self);
/* True when the interface has no readiness hook. Admission refuses on false (plan A10: a session
 * that cannot excite a spoke is refused, never quietly degraded to a hand pluck). */
bool truing_acoustic_ready(truing_acoustic_if_t *self, truing_reason_t *reason);

/* Which acoustic station a spoke must be brought to (SPEC §10A: a prompt names feature AND
 * station). The convention belongs to this subsystem, because it is the actuators' physical
 * reach that decides it: spoke 0 is the LEFT actuator's, and assignment alternates from there.
 * The orchestrator asks; it never learns the rule. Deliberately independent of §6.4.1 side
 * A/B and the solver's indexing_origin (see IMPLEMENTATION_NOTES, known limitation). */
truing_station_id_t truing_acoustic_station_for_spoke(uint8_t spoke_id);

/* ---- Stub: capability absent -------------------------------------------------- */
typedef struct {
    truing_clock_if_t clock;
    uint32_t          calls;
} truing_acoustic_stub_ctx_t;

void truing_acoustic_stub_init(truing_acoustic_if_t *self, truing_acoustic_stub_ctx_t *ctx, truing_clock_if_t clock);

/* ---- Synthetic: scripted per-spoke outcomes (SPEC §14.5) ------------------------ */
typedef struct {
    truing_clock_if_t      clock;
    uint8_t                n_spokes;
    truing_tension_model_t model_name;        /* the session-selected model being simulated */
    uint16_t               model_version;
    uint16_t               selection_rule_version;
    float                  snr_db;
    float                  tension_n[TRUING_MAX_SPOKES];
    float                  frequency_hz[TRUING_MAX_SPOKES];
    /* Scripted failure per spoke: status UNSET means "produce the scripted estimate". */
    truing_status_t        fail_status[TRUING_MAX_SPOKES];
    truing_reason_t        fail_reason[TRUING_MAX_SPOKES];
    bool                   cancel_requested;
    uint32_t               calls;
    uint32_t               cancelled_calls;
} truing_acoustic_synthetic_ctx_t;

void truing_acoustic_synthetic_init(truing_acoustic_if_t *self, truing_acoustic_synthetic_ctx_t *ctx,
                                    truing_clock_if_t clock, uint8_t n_spokes, truing_tension_model_t model_name,
                                    uint16_t model_version);
/* Script one spoke's outcome. */
void truing_acoustic_synthetic_set_spoke(truing_acoustic_synthetic_ctx_t *ctx, uint8_t spoke, float tension_n, float frequency_hz);
void truing_acoustic_synthetic_set_failure(truing_acoustic_synthetic_ctx_t *ctx, uint8_t spoke, truing_status_t status, truing_reason_t reason);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_ACOUSTIC_IF_H */
