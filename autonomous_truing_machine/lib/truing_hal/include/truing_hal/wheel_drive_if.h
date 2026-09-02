/**
 * @file wheel_drive_if.h
 * Wheel-drive ACTUATOR contract (SPEC §10A): the motor/driver that moves the wheel,
 * expressed strictly in ACTUATOR coordinates (steps). It knows nothing about
 * spokes, rim angles, stations, or how many steps make a wheel revolution — that
 * mapping belongs to the navigation implementation and its calibration.
 *
 * Expected first development implementation: TMC2209 (STEP/DIR + UART). It is a
 * development driver behind this boundary, NOT a committed final driver, and it
 * is not part of this phase; only the contract, a stub and a fake exist here.
 */
#ifndef TRUING_HAL_WHEEL_DRIVE_IF_H
#define TRUING_HAL_WHEEL_DRIVE_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_wheel_drive_if truing_wheel_drive_if_t;

struct truing_wheel_drive_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    truing_status_t (*enable)(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out);
    /* Command a relative move in actuator steps (sign = direction). Non-blocking. */
    truing_status_t (*move_relative)(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out);
    /* Controlled stop; the ABORT safe point for wheel motion (SPEC §12.3). */
    void (*stop)(truing_wheel_drive_if_t *self);
    bool (*is_moving)(const truing_wheel_drive_if_t *self);
    /* Actuator odometry in steps. NOT a wheel angle (SPEC §10A). */
    int32_t (*position_steps)(const truing_wheel_drive_if_t *self);
    void *ctx;
};

truing_status_t truing_wheel_drive_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out);
truing_status_t truing_wheel_drive_move_relative(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out);
void            truing_wheel_drive_stop(truing_wheel_drive_if_t *self);
bool            truing_wheel_drive_is_moving(const truing_wheel_drive_if_t *self);
int32_t         truing_wheel_drive_position_steps(const truing_wheel_drive_if_t *self);

/* ---- Stub: no drive hardware (Stage 1) ---------------------------------------- */
typedef struct {
    uint32_t calls;
} truing_wheel_drive_stub_ctx_t;

void truing_wheel_drive_stub_init(truing_wheel_drive_if_t *self, truing_wheel_drive_stub_ctx_t *ctx);

/* ---- Fake: instantaneous, deterministic (tests / simulation) ---------------------- */
typedef struct {
    bool     enabled;
    int32_t  position_steps;
    int32_t  last_move_steps;
    uint32_t moves;
    uint32_t stops;
} truing_wheel_drive_fake_ctx_t;

void truing_wheel_drive_fake_init(truing_wheel_drive_if_t *self, truing_wheel_drive_fake_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_WHEEL_DRIVE_IF_H */
