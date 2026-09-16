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

/* What actually happened during the most recent fire(), when an implementation can measure it.
 * Additive and optional: NULL fire_report on the vtable means "not supported", and a caller
 * must check for that before calling through it. pulse_us_measured is the actuator's own
 * report, not derived from anything the caller already has -- SPEC §6.2 keeps derivable
 * quantities out of the record, but this is not one. */
typedef struct {
    uint32_t pulse_us_measured; /* measured width of the last commanded pulse, in microseconds */
    bool     hardware_timed;    /* true if a timer/ISR set the pulse's end, not task scheduling */
} truing_pluck_fire_report_t;

struct truing_pluck_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    bool (*available)(truing_pluck_if_t *self);          /* an actuator is attached and ready */
    bool (*fire)(truing_pluck_if_t *self, float pulse_ms); /* command one excitation; false on failure */
    /* Optional; NULL if this implementation cannot report. Describes the fire() just completed,
     * not the one about to happen -- call it only after fire() has returned true. */
    bool (*fire_report)(truing_pluck_if_t *self, truing_pluck_fire_report_t *out);
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
