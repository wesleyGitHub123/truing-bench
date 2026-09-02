#include <stddef.h>
#include <string.h>

#include "truing_hal/intent_source_if.h"

bool truing_intent_source_poll(truing_intent_source_if_t *self, truing_intent_t *out)
{
    if (self == NULL || self->poll == NULL || out == NULL) {
        return false;
    }
    return self->poll(self, out);
}

static bool queue_poll(truing_intent_source_if_t *self, truing_intent_t *out)
{
    truing_intent_queue_ctx_t *ctx = (truing_intent_queue_ctx_t *)self->ctx;
    if (ctx == NULL || ctx->count == 0u) {
        return false;
    }
    *out = ctx->items[ctx->head];
    ctx->head = (uint8_t)((ctx->head + 1u) % TRUING_INTENT_QUEUE_CAPACITY);
    ctx->count--;
    return true;
}

void truing_intent_queue_init(truing_intent_source_if_t *self, truing_intent_queue_ctx_t *ctx)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    self->impl_name = "intent_queue";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->poll = queue_poll;
    self->ctx = ctx;
}

bool truing_intent_queue_push(truing_intent_queue_ctx_t *ctx, const truing_intent_t *intent)
{
    if (ctx == NULL || intent == NULL) {
        return false;
    }
    if (ctx->count >= TRUING_INTENT_QUEUE_CAPACITY) {
        ctx->dropped++;
        return false;
    }
    const uint8_t tail = (uint8_t)((ctx->head + ctx->count) % TRUING_INTENT_QUEUE_CAPACITY);
    ctx->items[tail] = *intent;
    ctx->count++;
    return true;
}

uint8_t truing_intent_queue_count(const truing_intent_queue_ctx_t *ctx)
{
    return ctx != NULL ? ctx->count : 0u;
}
