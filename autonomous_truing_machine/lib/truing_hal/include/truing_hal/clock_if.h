/**
 * @file clock_if.h
 * Monotonic millisecond clock source (SPEC §6.6: timestamps are ms since boot).
 * Core logic never calls a framework clock (P3); implementations receive one.
 */
#ifndef TRUING_HAL_CLOCK_IF_H
#define TRUING_HAL_CLOCK_IF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t (*now_ms)(void *ctx);
    void     *ctx;
} truing_clock_if_t;

static inline uint32_t truing_clock_now_ms(const truing_clock_if_t *clock)
{
    return (clock != 0 && clock->now_ms != 0) ? clock->now_ms(clock->ctx) : 0u;
}

/* A settable clock for host tests and simulation. */
typedef struct {
    uint32_t now_ms;
} truing_fake_clock_t;

void truing_fake_clock_init(truing_fake_clock_t *fc, truing_clock_if_t *out, uint32_t start_ms);
void truing_fake_clock_advance(truing_fake_clock_t *fc, uint32_t delta_ms);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_CLOCK_IF_H */
