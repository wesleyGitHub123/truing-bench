/**
 * @file pluck_if.h
 * Excitation actuator contract, internal to the acoustic subsystem (SPEC 9.1: a
 * sensor+actuator composite). `fire()` commands one excitation; whether anything
 * physical happens is a property of the implementation and is reported, not assumed.
 */
#ifndef TRUING_HAL_PLUCK_IF_H
#define TRUING_HAL_PLUCK_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_pluck_if truing_pluck_if_t;

struct truing_pluck_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    bool (*available)(truing_pluck_if_t *self);          /* an actuator is attached and ready */
    bool (*fire)(truing_pluck_if_t *self, float pulse_ms); /* command one excitation; false on failure */
    void *ctx;
};

/* Fake actuator for host tests and the self-play: counts commands, can be scripted to fail. */
typedef struct {
    bool     attached;
    bool     fail_next;
    uint32_t fires;
    float    last_pulse_ms;
} truing_pluck_fake_ctx_t;

void truing_pluck_fake_init(truing_pluck_if_t *self, truing_pluck_fake_ctx_t *ctx, bool attached);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_PLUCK_IF_H */
