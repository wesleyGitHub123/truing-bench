/**
 * @file intent_source_if.h
 * Operator intent is an EVENT, not a transport (SPEC §12.4). The state machine
 * polls intents from a source; what produces them (web UI, touchscreen, buttons,
 * a test queue) is a driver.
 */
#ifndef TRUING_HAL_INTENT_SOURCE_IF_H
#define TRUING_HAL_INTENT_SOURCE_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/operator_intent.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_intent_source_if truing_intent_source_if_t;

struct truing_intent_source_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    /* Non-blocking: returns true and fills `out` when an intent is pending. */
    bool (*poll)(truing_intent_source_if_t *self, truing_intent_t *out);
    void *ctx;
};

bool truing_intent_source_poll(truing_intent_source_if_t *self, truing_intent_t *out);

/* ---- Queue-backed synthetic source (tests / simulation) ---------------------------- */
#define TRUING_INTENT_QUEUE_CAPACITY 16u

typedef struct {
    truing_intent_t items[TRUING_INTENT_QUEUE_CAPACITY];
    uint8_t         head;
    uint8_t         count;
    uint32_t        dropped;
} truing_intent_queue_ctx_t;

void truing_intent_queue_init(truing_intent_source_if_t *self, truing_intent_queue_ctx_t *ctx);
/* Returns false (and counts a drop) when the queue is full. */
bool truing_intent_queue_push(truing_intent_queue_ctx_t *ctx, const truing_intent_t *intent);
uint8_t truing_intent_queue_count(const truing_intent_queue_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_INTENT_SOURCE_IF_H */
