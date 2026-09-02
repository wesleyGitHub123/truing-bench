/* Runout SYNTHETIC implementation: a snapshot-only table (SPEC §14.5).
 * Snapshot + tare are supported; streaming is reported `unavailable` /
 * NOT_IMPLEMENTED exactly as a snapshot-only device must (SPEC §10.1). */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing_hal/records.h"
#include "truing_hal/runout_if.h"

static uint32_t syn_caps(const truing_runout_if_t *self)
{
    (void)self;
    return TRUING_RUNOUT_CAP_SNAPSHOT | TRUING_RUNOUT_CAP_TARE;
}

static void syn_snapshot(truing_runout_if_t *self, uint8_t rim_index, float rim_angle_rad,
                         uint8_t cycle_index, truing_runout_measurement_t *out)
{
    truing_runout_synthetic_ctx_t *ctx = (truing_runout_synthetic_ctx_t *)self->ctx;
    if (out == NULL) {
        return;
    }
    if (ctx == NULL) {
        truing_hal_fill_unavailable_runout(out, TRUING_REASON_NOT_IMPLEMENTED, rim_angle_rad, cycle_index, 0u, self->source_impl);
        return;
    }
    ctx->snapshot_calls++;
    const uint32_t now = truing_clock_now_ms(&ctx->clock);
    if (rim_index >= ctx->n_rim_angles) {
        truing_hal_fill_unavailable_runout(out, TRUING_REASON_VALUE_OUT_OF_RANGE, rim_angle_rad, cycle_index, now, self->source_impl);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->meta.cycle_index = cycle_index;
    out->meta.timestamp_ms = now;
    out->meta.source_impl = self->source_impl;
    out->rim_angle_rad = rim_angle_rad;
    if (ctx->fail_status[rim_index] != TRUING_STATUS_UNSET) {
        out->meta.status = ctx->fail_status[rim_index];
        out->meta.reason_code = ctx->fail_reason[rim_index];
        out->lateral_mm = NAN;
        out->radial_mm = NAN;
        return;
    }
    out->meta.status = TRUING_STATUS_VALID;
    out->meta.reason_code = TRUING_REASON_NONE;
    out->lateral_mm = ctx->lateral_mm[rim_index] - (ctx->tared ? ctx->tare_lateral_mm : 0.0f);
    out->radial_mm = ctx->radial_mm[rim_index] - (ctx->tared ? ctx->tare_radial_mm : 0.0f);
}

static truing_status_t syn_stream(truing_runout_if_t *self, truing_runout_stream_cb_t cb, void *user,
                                  uint32_t max_samples, truing_reason_t *reason_out)
{
    (void)cb;
    (void)user;
    (void)max_samples;
    truing_runout_synthetic_ctx_t *ctx = (truing_runout_synthetic_ctx_t *)self->ctx;
    if (ctx != NULL) {
        ctx->stream_calls++;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return TRUING_STATUS_UNAVAILABLE;
}

static truing_status_t syn_tare(truing_runout_if_t *self, truing_reason_t *reason_out)
{
    truing_runout_synthetic_ctx_t *ctx = (truing_runout_synthetic_ctx_t *)self->ctx;
    if (ctx == NULL || ctx->n_rim_angles == 0u) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    /* Zero the gauges at the reference position: rim index 0 (valve stem, SPEC §6.4). */
    ctx->tare_lateral_mm = ctx->lateral_mm[0];
    ctx->tare_radial_mm = ctx->radial_mm[0];
    ctx->tared = true;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

void truing_runout_synthetic_init(truing_runout_if_t *self, truing_runout_synthetic_ctx_t *ctx,
                                  truing_clock_if_t clock, uint8_t n_rim_angles)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock = clock;
    ctx->n_rim_angles = n_rim_angles <= TRUING_MAX_RIM_ANGLES ? n_rim_angles : (uint8_t)TRUING_MAX_RIM_ANGLES;
    for (uint8_t k = 0; k < TRUING_MAX_RIM_ANGLES; ++k) {
        ctx->lateral_mm[k] = NAN;   /* unscripted indices fail validation rather than read as zero */
        ctx->radial_mm[k] = NAN;
    }
    self->impl_name = "runout_synthetic";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->capabilities = syn_caps;
    self->read_snapshot = syn_snapshot;
    self->stream_samples = syn_stream;
    self->tare = syn_tare;
    self->ctx = ctx;
}

void truing_runout_synthetic_set_index(truing_runout_synthetic_ctx_t *ctx, uint8_t rim_index, float lateral_mm, float radial_mm)
{
    if (ctx == NULL || rim_index >= TRUING_MAX_RIM_ANGLES) {
        return;
    }
    ctx->lateral_mm[rim_index] = lateral_mm;
    ctx->radial_mm[rim_index] = radial_mm;
    ctx->fail_status[rim_index] = TRUING_STATUS_UNSET;
    ctx->fail_reason[rim_index] = TRUING_REASON_NONE;
}

void truing_runout_synthetic_set_failure(truing_runout_synthetic_ctx_t *ctx, uint8_t rim_index, truing_status_t status, truing_reason_t reason)
{
    if (ctx == NULL || rim_index >= TRUING_MAX_RIM_ANGLES) {
        return;
    }
    ctx->fail_status[rim_index] = status;
    ctx->fail_reason[rim_index] = reason;
}
