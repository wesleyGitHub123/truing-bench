/* Runout MANUAL-ENTRY implementation — the Stage 1 primary (SPEC §10.2).
 *
 * The orchestrator enters WAIT_FOR_OPERATOR, the operator reads the dial gauges and
 * sends SUBMIT_RUNOUT(lateral, radial) carrying the active wait_id, the orchestrator
 * hands the values to truing_runout_manual_submit(), and only then calls
 * read_snapshot(). Nothing here blocks on a network or browser operation. This is
 * a real implementation that stays alongside any future automated sensor. */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing_hal/records.h"
#include "truing_hal/runout_if.h"

static uint32_t manual_caps(const truing_runout_if_t *self)
{
    (void)self;
    return TRUING_RUNOUT_CAP_SNAPSHOT | TRUING_RUNOUT_CAP_TARE | TRUING_RUNOUT_CAP_MANUAL_ENTRY;
}

static void manual_snapshot(truing_runout_if_t *self, uint8_t rim_index, float rim_angle_rad,
                            uint8_t cycle_index, truing_runout_measurement_t *out)
{
    truing_runout_manual_ctx_t *ctx = (truing_runout_manual_ctx_t *)self->ctx;
    if (out == NULL) {
        return;
    }
    if (ctx == NULL) {
        truing_hal_fill_unavailable_runout(out, TRUING_REASON_NOT_IMPLEMENTED, rim_angle_rad, cycle_index, 0u, self->source_impl);
        return;
    }
    ctx->snapshot_calls++;
    const uint32_t now = truing_clock_now_ms(&ctx->clock);
    if (!ctx->pending || ctx->pending_rim_index != rim_index) {
        /* No operator entry for this index: report the absence, never a value. */
        truing_hal_fill_unavailable_runout(out, TRUING_REASON_SENSOR_TIMEOUT, rim_angle_rad, cycle_index, now, self->source_impl);
        return;
    }
    ctx->pending = false;
    memset(out, 0, sizeof(*out));
    out->meta.cycle_index = cycle_index;
    out->meta.timestamp_ms = now;
    out->meta.source_impl = self->source_impl;
    out->rim_angle_rad = rim_angle_rad;
    if (!isfinite(ctx->pending_lateral_mm) || !isfinite(ctx->pending_radial_mm)) {
        out->meta.status = TRUING_STATUS_REJECTED;
        out->meta.reason_code = TRUING_REASON_VALUE_OUT_OF_RANGE;
        out->lateral_mm = NAN;
        out->radial_mm = NAN;
        return;
    }
    out->meta.status = TRUING_STATUS_VALID;
    out->meta.reason_code = TRUING_REASON_NONE;
    out->lateral_mm = ctx->pending_lateral_mm;
    out->radial_mm = ctx->pending_radial_mm;
}

static truing_status_t manual_stream(truing_runout_if_t *self, truing_runout_stream_cb_t cb, void *user,
                                     uint32_t max_samples, truing_reason_t *reason_out)
{
    (void)self;
    (void)cb;
    (void)user;
    (void)max_samples;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;   /* SPEC §10.1: snapshot-only device */
    }
    return TRUING_STATUS_UNAVAILABLE;
}

static truing_status_t manual_tare(truing_runout_if_t *self, truing_reason_t *reason_out)
{
    truing_runout_manual_ctx_t *ctx = (truing_runout_manual_ctx_t *)self->ctx;
    if (ctx == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    /* The operator zeroes the physical gauges at the reference (SPEC §10.3); entries are
     * relative to that zero. The implementation only records that the step was performed. */
    ctx->tared = true;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

void truing_runout_manual_init(truing_runout_if_t *self, truing_runout_manual_ctx_t *ctx, truing_clock_if_t clock)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock = clock;
    self->impl_name = "runout_manual";
    self->source_impl = TRUING_SOURCE_REAL;
    self->capabilities = manual_caps;
    self->read_snapshot = manual_snapshot;
    self->stream_samples = manual_stream;
    self->tare = manual_tare;
    self->ctx = ctx;
}

bool truing_runout_manual_submit(truing_runout_if_t *self, uint8_t rim_index, float lateral_mm, float radial_mm)
{
    if (self == NULL || self->ctx == NULL || (truing_runout_capabilities(self) & TRUING_RUNOUT_CAP_MANUAL_ENTRY) == 0u) {
        return false;
    }
    truing_runout_manual_ctx_t *ctx = (truing_runout_manual_ctx_t *)self->ctx;
    ctx->pending = true;
    ctx->pending_rim_index = rim_index;
    ctx->pending_lateral_mm = lateral_mm;
    ctx->pending_radial_mm = radial_mm;
    ctx->submissions++;
    return true;
}
