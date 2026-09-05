/**
 * @file telemetry_if.h
 * Telemetry sink (SPEC §12.2): one-way, best-effort, observational.
 *
 *   - emit() NEVER blocks the control path; it returns false when the event was dropped.
 *   - No delivery guarantee, no replay, no reconnect recovery.
 *   - Telemetry failure never affects control flow (SPEC §13.3).
 *
 * Authoritative provenance for the CURRENT cycle lives in RAM and is queried on
 * demand (P6); it is never reconstructed from this stream.
 */
#ifndef TRUING_HAL_TELEMETRY_IF_H
#define TRUING_HAL_TELEMETRY_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/operator_intent.h"
#include "truing/state_ids.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_EVT_UNSET = 0,
    TRUING_EVT_STATE_TRANSITION,
    TRUING_EVT_WAIT_ISSUED,
    TRUING_EVT_INTENT_REJECTED,
    TRUING_EVT_MEASUREMENT_RESULT,
    TRUING_EVT_TERMINAL_RESULT,
    TRUING_EVT_NAVIGATION,        /* wheel positioning request / outcome (SPEC §10A) */
    TRUING_EVT_LOG,
    TRUING_EVT_ACOUSTIC_PHASE,    /* lifecycle INSIDE one acoustic call (SPEC §9.1) */
    TRUING_EVT__COUNT
} truing_event_kind_t;

typedef enum {
    TRUING_EVT_CHANNEL_TENSION = 0,
    TRUING_EVT_CHANNEL_RUNOUT,
} truing_event_channel_t;

/* One acoustic measurement is a single call by contract (SPEC §9.1), but from the station it is
 * two very different things back to back: a window in which a person must pluck, and seconds of
 * arithmetic during which they must not. Nothing outside the subsystem could tell those apart,
 * so an operator was shown one card for both and had to guess. These say which is happening.
 *
 * Purely observational, best-effort like all telemetry (SPEC §12.2): dropping every one of them
 * changes no measurement and no timing. The capture window is opened and closed by the firmware
 * on its own clock; a browser that never hears about it still gets the same result. */
typedef enum {
    TRUING_ACOUSTIC_PHASE_LISTENING = 0,   /* the capture window is OPEN: pluck now */
    TRUING_ACOUSTIC_PHASE_ONSET_DETECTED,  /* an excitation was found in the capture */
    TRUING_ACOUSTIC_PHASE_ANALYZING,       /* the window is closed; the FFT is running */
    TRUING_ACOUSTIC_PHASE__COUNT
} truing_acoustic_phase_t;

/* Where the excitation comes from, as a fact reported by the firmware rather than a guess made
 * by the page. The actuator case has no implementation yet; carrying it now is what lets one
 * arrive as a pluck_if implementation plus one wiring line, with no wire or UI change. */
typedef enum {
    TRUING_EXCITATION_NONE = 0,   /* no actuator, no station: replayed or bring-up audio */
    TRUING_EXCITATION_HAND,       /* a person plucks at the station */
    TRUING_EXCITATION_ACTUATOR,   /* an attached actuator was commanded */
    TRUING_EXCITATION__COUNT
} truing_excitation_t;

#define TRUING_EVT_TEXT_MAX 40u

typedef struct {
    truing_event_kind_t kind;
    uint32_t            timestamp_ms;
    uint8_t             cycle_index;
    union {
        struct {
            truing_state_t from;
            truing_state_t to;
        } transition;
        truing_wait_prompt_t wait;
        struct {
            truing_intent_type_t    intent;
            truing_intent_verdict_t verdict;
            uint32_t                wait_id;
        } rejected;
        struct {
            truing_event_channel_t channel;
            uint8_t                index;
            truing_status_t        status;
            truing_reason_t        reason;
            float                  value_a;   /* tension_n | lateral_mm */
            float                  value_b;   /* selected_frequency_hz | radial_mm */
        } measurement;
        truing_terminal_result_t terminal;
        struct {
            uint8_t target_kind;     /* truing_nav_target_kind_t */
            uint8_t index;
            float   angle_rad;       /* wheel-coordinate target angle */
            uint8_t station;         /* truing_station_id_t */
            uint8_t outcome;         /* truing_nav_outcome_t */
            float   rotation_rad;    /* believed wheel rotation after the operation, or NaN */
        } navigation;
        struct {
            uint8_t  phase;            /* truing_acoustic_phase_t */
            uint8_t  spoke_index;
            uint8_t  excitation;       /* truing_excitation_t */
            bool     pluck_commanded;  /* an actuator was fired for this attempt */
            uint32_t window_ms;        /* LISTENING: how long the window stays open */
            uint32_t attempt;          /* 1-based, counting consecutive calls for this spoke */
        } acoustic;
        char text[TRUING_EVT_TEXT_MAX];
    } u;
} truing_telemetry_event_t;

typedef struct truing_telemetry_if truing_telemetry_if_t;

struct truing_telemetry_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    bool (*emit)(truing_telemetry_if_t *self, const truing_telemetry_event_t *event);
    void *ctx;
};

/* Null-safe; a missing sink simply drops. Never blocks. */
bool truing_telemetry_emit(truing_telemetry_if_t *self, const truing_telemetry_event_t *event);

/* ---- Ring-buffer sink: drop-on-full (SPEC §12.2) ---------------------------------- */
#define TRUING_TELEMETRY_RING_CAPACITY 64u

typedef struct {
    truing_telemetry_event_t items[TRUING_TELEMETRY_RING_CAPACITY];
    uint16_t head;
    uint16_t count;
    uint32_t emitted;
    uint32_t dropped;
} truing_telemetry_ring_ctx_t;

void truing_telemetry_ring_init(truing_telemetry_if_t *self, truing_telemetry_ring_ctx_t *ctx);
/* Drain side (a low-priority task on the target; tests on the host). */
bool truing_telemetry_ring_pop(truing_telemetry_ring_ctx_t *ctx, truing_telemetry_event_t *out);
uint16_t truing_telemetry_ring_count(const truing_telemetry_ring_ctx_t *ctx);

const char *truing_event_kind_str(truing_event_kind_t k);
const char *truing_acoustic_phase_str(truing_acoustic_phase_t p);
const char *truing_excitation_str(truing_excitation_t e);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_TELEMETRY_IF_H */
