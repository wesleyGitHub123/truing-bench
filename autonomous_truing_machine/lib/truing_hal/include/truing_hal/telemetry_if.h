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
    TRUING_EVT__COUNT
} truing_event_kind_t;

typedef enum {
    TRUING_EVT_CHANNEL_TENSION = 0,
    TRUING_EVT_CHANNEL_RUNOUT,
} truing_event_channel_t;

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

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_TELEMETRY_IF_H */
