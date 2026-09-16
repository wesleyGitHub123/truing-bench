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
     * the excitation profile never validates a non-positive pulse, so one arriving here is a bug. */
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

/* Deterministic by construction: the commanded width, exactly, converted to microseconds.
 * hardware_timed is false -- nothing here is timed by real hardware, and saying otherwise would
 * misrepresent what a host test or self-play run is actually exercising. Fails before any fire
 * has happened, same as asking for a report on an actuator that has never been used. */
static bool fake_fire_report(truing_pluck_if_t *self, truing_pluck_fire_report_t *out)
{
    const truing_pluck_fake_ctx_t *c = (const truing_pluck_fake_ctx_t *)self->ctx;
    if (c == NULL || out == NULL || c->fires == 0u) {
        return false;
    }
    out->pulse_us_measured = (uint32_t)(c->last_pulse_ms * 1000.0f);
    out->hardware_timed = false;
    return true;
}

void truing_pluck_fake_init(truing_pluck_if_t *self, truing_pluck_fake_ctx_t *ctx, bool attached)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    /* Zeroed first so every field this init does not explicitly set -- including any added
     * later -- starts at a safe default rather than whatever was on the caller's stack. */
    memset(self, 0, sizeof(*self));
    memset(ctx, 0, sizeof(*ctx));
    ctx->attached = attached;
    self->impl_name = "pluck_fake";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->available = fake_available;
    self->fire = fake_fire;
    self->fire_report = fake_fire_report;
    self->ctx = ctx;
}
