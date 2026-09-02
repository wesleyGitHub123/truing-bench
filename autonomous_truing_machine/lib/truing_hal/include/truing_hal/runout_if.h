/**
 * @file runout_if.h
 * Runout subsystem interface (SPEC §10):
 *
 *   read_snapshot(rim_angle) -> RunoutMeasurement   (lateral + radial)
 *   stream_samples(...)      -> sample stream       (DEFINED, NOT IMPLEMENTED)
 *   tare()                                          (zero the gauges at setup)
 *
 * Snapshot and streaming are separate capabilities; an implementation may
 * support only one and MUST report the other as `unavailable` / NOT_IMPLEMENTED.
 * Capability query is part of the contract so a caller can refuse cleanly at
 * setup rather than fail mid-adjustment.
 *
 * The orchestrator owns the indexing loop and pulls readings at known rim
 * angles; the subsystem never decides when or where to read.
 */
#ifndef TRUING_HAL_RUNOUT_IF_H
#define TRUING_HAL_RUNOUT_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/measurements.h"
#include "truing/status.h"
#include "truing_hal/clock_if.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_RUNOUT_CAP_SNAPSHOT (1u << 0)
#define TRUING_RUNOUT_CAP_STREAM   (1u << 1)
#define TRUING_RUNOUT_CAP_TARE     (1u << 2)

typedef struct {
    float    lateral_mm;
    float    radial_mm;
    uint32_t timestamp_ms;
} truing_runout_sample_t;

/* Return false to stop the stream early. */
typedef bool (*truing_runout_stream_cb_t)(const truing_runout_sample_t *sample, void *user);

typedef struct truing_runout_if truing_runout_if_t;

struct truing_runout_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    uint32_t (*capabilities)(const truing_runout_if_t *self);
    void (*read_snapshot)(truing_runout_if_t *self, uint8_t rim_index, float rim_angle_rad,
                          uint8_t cycle_index, truing_runout_measurement_t *out);
    truing_status_t (*stream_samples)(truing_runout_if_t *self, truing_runout_stream_cb_t cb, void *user,
                                      uint32_t max_samples, truing_reason_t *reason_out);
    truing_status_t (*tare)(truing_runout_if_t *self, truing_reason_t *reason_out);
    void *ctx;
};

/* Null-safe wrappers; missing slots report `unavailable` / NOT_IMPLEMENTED (P2). */
uint32_t        truing_runout_capabilities(const truing_runout_if_t *self);
void            truing_runout_read_snapshot(truing_runout_if_t *self, uint8_t rim_index, float rim_angle_rad,
                                            uint8_t cycle_index, truing_runout_measurement_t *out);
truing_status_t truing_runout_stream_samples(truing_runout_if_t *self, truing_runout_stream_cb_t cb, void *user,
                                             uint32_t max_samples, truing_reason_t *reason_out);
truing_status_t truing_runout_tare(truing_runout_if_t *self, truing_reason_t *reason_out);

/* ---- Stub: no capability ----------------------------------------------------- */
typedef struct {
    truing_clock_if_t clock;
    uint32_t          calls;
} truing_runout_stub_ctx_t;

void truing_runout_stub_init(truing_runout_if_t *self, truing_runout_stub_ctx_t *ctx, truing_clock_if_t clock);

/* ---- Synthetic: snapshot-only table (SPEC §14.5) --------------------------------- */
typedef struct {
    truing_clock_if_t clock;
    uint8_t           n_rim_angles;
    float             lateral_mm[TRUING_MAX_RIM_ANGLES];
    float             radial_mm[TRUING_MAX_RIM_ANGLES];
    float             tare_lateral_mm;   /* reference subtracted after tare() */
    float             tare_radial_mm;
    bool              tared;
    truing_status_t   fail_status[TRUING_MAX_RIM_ANGLES];
    truing_reason_t   fail_reason[TRUING_MAX_RIM_ANGLES];
    uint32_t          snapshot_calls;
    uint32_t          stream_calls;
} truing_runout_synthetic_ctx_t;

void truing_runout_synthetic_init(truing_runout_if_t *self, truing_runout_synthetic_ctx_t *ctx,
                                  truing_clock_if_t clock, uint8_t n_rim_angles);
void truing_runout_synthetic_set_index(truing_runout_synthetic_ctx_t *ctx, uint8_t rim_index, float lateral_mm, float radial_mm);
void truing_runout_synthetic_set_failure(truing_runout_synthetic_ctx_t *ctx, uint8_t rim_index, truing_status_t status, truing_reason_t reason);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_RUNOUT_IF_H */
