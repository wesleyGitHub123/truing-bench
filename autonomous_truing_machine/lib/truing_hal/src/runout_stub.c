/* Runout STUB: no capability at all. Every operation reports `unavailable` /
 * NOT_IMPLEMENTED (SPEC P2, §10.1). Also hosts the null-safe wrappers. */
#include <stddef.h>

#include "truing_hal/records.h"
#include "truing_hal/runout_if.h"

static uint32_t stub_caps(const truing_runout_if_t *self)
{
    (void)self;
    return 0u;
}

static void stub_snapshot(truing_runout_if_t *self, uint8_t rim_index, float rim_angle_rad,
                          uint8_t cycle_index, truing_runout_measurement_t *out)
{
    (void)rim_index;
    truing_runout_stub_ctx_t *ctx = (truing_runout_stub_ctx_t *)self->ctx;
    uint32_t now = 0u;
    if (ctx != NULL) {
        ctx->calls++;
        now = truing_clock_now_ms(&ctx->clock);
    }
    truing_hal_fill_unavailable_runout(out, TRUING_REASON_NOT_IMPLEMENTED, rim_angle_rad, cycle_index, now, self->source_impl);
}

static truing_status_t stub_stream(truing_runout_if_t *self, truing_runout_stream_cb_t cb, void *user,
                                   uint32_t max_samples, truing_reason_t *reason_out)
{
    (void)self;
    (void)cb;
    (void)user;
    (void)max_samples;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return TRUING_STATUS_UNAVAILABLE;
}

static truing_status_t stub_tare(truing_runout_if_t *self, truing_reason_t *reason_out)
{
    (void)self;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return TRUING_STATUS_UNAVAILABLE;
}

void truing_runout_stub_init(truing_runout_if_t *self, truing_runout_stub_ctx_t *ctx, truing_clock_if_t clock)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    ctx->clock = clock;
    ctx->calls = 0u;
    self->impl_name = "runout_stub";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->capabilities = stub_caps;
    self->read_snapshot = stub_snapshot;
    self->stream_samples = stub_stream;
    self->tare = stub_tare;
    self->ctx = ctx;
}

/* ---- null-safe wrappers ----------------------------------------------------------- */
uint32_t truing_runout_capabilities(const truing_runout_if_t *self)
{
    return (self != NULL && self->capabilities != NULL) ? self->capabilities(self) : 0u;
}

void truing_runout_read_snapshot(truing_runout_if_t *self, uint8_t rim_index, float rim_angle_rad,
                                 uint8_t cycle_index, truing_runout_measurement_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->read_snapshot == NULL) {
        truing_hal_fill_unavailable_runout(out, TRUING_REASON_NOT_IMPLEMENTED, rim_angle_rad, cycle_index, 0u,
                                           self != NULL ? self->source_impl : TRUING_SOURCE_SYNTHETIC);
        return;
    }
    self->read_snapshot(self, rim_index, rim_angle_rad, cycle_index, out);
}

truing_status_t truing_runout_stream_samples(truing_runout_if_t *self, truing_runout_stream_cb_t cb, void *user,
                                             uint32_t max_samples, truing_reason_t *reason_out)
{
    if (self == NULL || self->stream_samples == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return self->stream_samples(self, cb, user, max_samples, reason_out);
}

truing_status_t truing_runout_tare(truing_runout_if_t *self, truing_reason_t *reason_out)
{
    if (self == NULL || self->tare == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return self->tare(self, reason_out);
}
