#include <stddef.h>
#include <string.h>

#include "truing_hal/telemetry_if.h"

static const char *const k_kind_str[TRUING_EVT__COUNT] = {
    "UNSET", "STATE_TRANSITION", "WAIT_ISSUED", "INTENT_REJECTED", "MEASUREMENT_RESULT",
    "TERMINAL_RESULT", "NAVIGATION", "LOG", "ACOUSTIC_PHASE",
};

static const char *const k_phase_str[TRUING_ACOUSTIC_PHASE__COUNT] = {
    "LISTENING", "ONSET_DETECTED", "ANALYZING",
};

static const char *const k_excitation_str[TRUING_EXCITATION__COUNT] = {
    "NONE", "HAND", "ACTUATOR",
};

const char *truing_acoustic_phase_str(truing_acoustic_phase_t p)
{
    return (unsigned)p < TRUING_ACOUSTIC_PHASE__COUNT ? k_phase_str[p] : "?";
}

const char *truing_excitation_str(truing_excitation_t e)
{
    return (unsigned)e < TRUING_EXCITATION__COUNT ? k_excitation_str[e] : "?";
}

const char *truing_event_kind_str(truing_event_kind_t k)
{
    return (unsigned)k < TRUING_EVT__COUNT ? k_kind_str[k] : "?";
}

bool truing_telemetry_emit(truing_telemetry_if_t *self, const truing_telemetry_event_t *event)
{
    if (self == NULL || self->emit == NULL || event == NULL) {
        return false;   /* dropped; control flow is unaffected (SPEC §13.3) */
    }
    return self->emit(self, event);
}

static bool ring_emit(truing_telemetry_if_t *self, const truing_telemetry_event_t *event)
{
    truing_telemetry_ring_ctx_t *ctx = (truing_telemetry_ring_ctx_t *)self->ctx;
    if (ctx == NULL) {
        return false;
    }
    if (ctx->count >= TRUING_TELEMETRY_RING_CAPACITY) {
        ctx->dropped++;      /* drop-on-full: never block, never overwrite in flight */
        return false;
    }
    const uint16_t tail = (uint16_t)((ctx->head + ctx->count) % TRUING_TELEMETRY_RING_CAPACITY);
    ctx->items[tail] = *event;
    ctx->count++;
    ctx->emitted++;
    return true;
}

void truing_telemetry_ring_init(truing_telemetry_if_t *self, truing_telemetry_ring_ctx_t *ctx)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    self->impl_name = "telemetry_ring";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->emit = ring_emit;
    self->ctx = ctx;
}

bool truing_telemetry_ring_pop(truing_telemetry_ring_ctx_t *ctx, truing_telemetry_event_t *out)
{
    if (ctx == NULL || out == NULL || ctx->count == 0u) {
        return false;
    }
    *out = ctx->items[ctx->head];
    ctx->head = (uint16_t)((ctx->head + 1u) % TRUING_TELEMETRY_RING_CAPACITY);
    ctx->count--;
    return true;
}

uint16_t truing_telemetry_ring_count(const truing_telemetry_ring_ctx_t *ctx)
{
    return ctx != NULL ? ctx->count : 0u;
}
