#include "truing_hal/spoke_sensor_if.h"

#include <string.h>

/* The fake behind spoke_sensor_if.h: a scripted queue of beam edges, for host tests of the planned
 * automatic roller mode. No real implementation exists yet (the laser module is not delivered). */
static bool fake_available(truing_spoke_sensor_if_t *self)
{
    return ((truing_spoke_sensor_fake_ctx_t *)self->ctx)->fitted;
}

static bool fake_blocked(truing_spoke_sensor_if_t *self)
{
    const truing_spoke_sensor_fake_ctx_t *c = (truing_spoke_sensor_fake_ctx_t *)self->ctx;
    return c->fitted && c->blocked;
}

static bool fake_next_event(truing_spoke_sensor_if_t *self, truing_spoke_event_t *out)
{
    truing_spoke_sensor_fake_ctx_t *c = (truing_spoke_sensor_fake_ctx_t *)self->ctx;
    if (!c->fitted || c->count == 0u || out == NULL) {
        return false;
    }
    *out = c->queue[c->head];
    c->head = (uint8_t)((c->head + 1u) % TRUING_SPOKE_SENSOR_FAKE_QUEUE);
    c->count--;
    return true;
}

void truing_spoke_sensor_fake_init(truing_spoke_sensor_if_t *self, truing_spoke_sensor_fake_ctx_t *ctx, bool fitted)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fitted = fitted;
    self->impl_name = "spoke_sensor_fake";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->available = fake_available;
    self->blocked = fake_blocked;
    self->next_event = fake_next_event;
    self->ctx = ctx;
}

bool truing_spoke_sensor_fake_push(truing_spoke_sensor_fake_ctx_t *ctx, truing_spoke_edge_t edge, uint32_t t_us,
                                   int32_t drive_steps)
{
    if (ctx->count == TRUING_SPOKE_SENSOR_FAKE_QUEUE) {
        ctx->dropped++;
        return false;
    }
    const uint8_t tail = (uint8_t)((ctx->head + ctx->count) % TRUING_SPOKE_SENSOR_FAKE_QUEUE);
    ctx->queue[tail] = (truing_spoke_event_t){ .edge = edge, .t_us = t_us, .drive_steps = drive_steps };
    ctx->count++;
    ctx->blocked = edge == TRUING_SPOKE_EDGE_ENTER;
    return true;
}
