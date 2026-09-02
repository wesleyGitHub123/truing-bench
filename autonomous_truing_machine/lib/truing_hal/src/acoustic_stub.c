/* Acoustic subsystem STUB: the capability is absent (Phase 1f not yet built).
 * SPEC P2: returns `unavailable` with NOT_IMPLEMENTED — never a neutral value.
 * It is labelled `synthetic` in provenance because it is not real hardware; any
 * session using it is flagged contains_non_real_implementations (SPEC §6.2). */
#include <stddef.h>

#include "truing_hal/acoustic_if.h"
#include "truing_hal/records.h"

static void stub_measure(truing_acoustic_if_t *self, uint8_t spoke_id,
                         const truing_wheel_class_config_t *wheel_geometry,
                         uint8_t cycle_index, truing_tension_estimate_t *out)
{
    (void)spoke_id;
    (void)wheel_geometry;
    truing_acoustic_stub_ctx_t *ctx = (truing_acoustic_stub_ctx_t *)self->ctx;
    uint32_t now = 0u;
    if (ctx != NULL) {
        ctx->calls++;
        now = truing_clock_now_ms(&ctx->clock);
    }
    truing_hal_fill_unavailable_estimate(out, TRUING_REASON_NOT_IMPLEMENTED, cycle_index, now, self->source_impl);
}

static void stub_cancel(truing_acoustic_if_t *self)
{
    (void)self;
}

void truing_acoustic_stub_init(truing_acoustic_if_t *self, truing_acoustic_stub_ctx_t *ctx, truing_clock_if_t clock)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    ctx->clock = clock;
    ctx->calls = 0u;
    self->impl_name = "acoustic_stub";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->measure_spoke_tension = stub_measure;
    self->request_cancel = stub_cancel;
    self->ctx = ctx;
}

/* ---- null-safe wrappers ----------------------------------------------------------- */
void truing_acoustic_measure(truing_acoustic_if_t *self, uint8_t spoke_id,
                             const truing_wheel_class_config_t *wheel_geometry,
                             uint8_t cycle_index, truing_tension_estimate_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->measure_spoke_tension == NULL) {
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_NOT_IMPLEMENTED, cycle_index, 0u,
                                             self != NULL ? self->source_impl : TRUING_SOURCE_SYNTHETIC);
        return;
    }
    self->measure_spoke_tension(self, spoke_id, wheel_geometry, cycle_index, out);
}

void truing_acoustic_request_cancel(truing_acoustic_if_t *self)
{
    if (self != NULL && self->request_cancel != NULL) {
        self->request_cancel(self);
    }
}
