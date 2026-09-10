#include <stddef.h>
#include <string.h>

#include "truing_hal/pluck_if.h"

static bool fake_available(truing_pluck_if_t *self)
{
    const truing_pluck_fake_ctx_t *c = (const truing_pluck_fake_ctx_t *)self->ctx;
    return c != NULL && c->attached;
}

static bool fake_fire(truing_pluck_if_t *self, float pulse_ms)
{
    truing_pluck_fake_ctx_t *c = (truing_pluck_fake_ctx_t *)self->ctx;
    /* Same edge-case contract as pluck_gpio: a non-positive width is refused, not clipped -
     * excitation_pulse_ms == 0 is the chain profile's own "no actuator commanded". */
    if (c == NULL || !c->attached || !(pulse_ms > 0.0f)) {
        return false;
    }
    if (c->fail_next) {
        c->fail_next = false;
        return false;
    }
    c->fires++;
    c->last_pulse_ms = pulse_ms;
    return true;
}

void truing_pluck_fake_init(truing_pluck_if_t *self, truing_pluck_fake_ctx_t *ctx, bool attached)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->attached = attached;
    self->impl_name = "pluck_fake";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->available = fake_available;
    self->fire = fake_fire;
    self->ctx = ctx;
}
