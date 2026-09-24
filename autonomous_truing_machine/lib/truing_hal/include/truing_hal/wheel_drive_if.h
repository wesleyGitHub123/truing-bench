/**
 * @file wheel_drive_if.h
 * Wheel-drive ACTUATOR contract (SPEC §10A): the motor/driver that moves the wheel,
 * expressed strictly in ACTUATOR coordinates (steps). It knows nothing about
 * spokes, rim angles, stations, or how many steps make a wheel revolution — that
 * mapping belongs to the navigation implementation and its calibration.
 *
 * Expected first development implementation: TMC2209 (STEP/DIR + UART), in
 * src/wheel_drive_tmc2209.c. It is a development driver behind this boundary, NOT a
 * committed final driver. A stub and a fake live here.
 *
 * Semantics every implementation keeps (the navigation implementation relies on them):
 *   - position_steps() counts the pulses ACTUALLY emitted, including after stop() cuts a
 *     move short. It is commanded motion, never evidence the wheel followed (SPEC §10A.5).
 *   - move_relative() while a move is in progress is refused (VALUE_OUT_OF_RANGE); it never
 *     queues or merges.
 *   - stop() is a controlled deceleration; is_moving() stays true until the last pulse.
 *   - fault() latches: once the driver reports it cannot vouch for executing its motion
 *     (over-temperature, short, a DIAG assertion), it stays faulted until the next
 *     enable(true). `detail` is implementation-specific diagnostic data, for logs only.
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
    /* Latched driver fault; optional (NULL = the implementation cannot detect one). */
    bool (*fault)(const truing_wheel_drive_if_t *self, uint32_t *detail_out);
    void *ctx;
};

truing_status_t truing_wheel_drive_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out);
truing_status_t truing_wheel_drive_move_relative(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out);
void            truing_wheel_drive_stop(truing_wheel_drive_if_t *self);
bool            truing_wheel_drive_is_moving(const truing_wheel_drive_if_t *self);
int32_t         truing_wheel_drive_position_steps(const truing_wheel_drive_if_t *self);
bool            truing_wheel_drive_fault(const truing_wheel_drive_if_t *self, uint32_t *detail_out);

/* ---- Stub: no drive hardware (Stage 1) ---------------------------------------- */
typedef struct {
    uint32_t calls;
} truing_wheel_drive_stub_ctx_t;

void truing_wheel_drive_stub_init(truing_wheel_drive_if_t *self, truing_wheel_drive_stub_ctx_t *ctx);

/* ---- Fake: deterministic (tests / simulation) ------------------------------------- */
/* By default a move completes instantly. Scripting, all off by default:
 *   polls_per_move  a move stays in progress for that many is_moving() calls, then lands
 *   stall           a move reports is_moving() until stop() (a hung drive)
 *   fault_next      the next move latches a fault (detail = fault_detail) as it starts
 * A stop() mid-move lands half of the remaining steps, standing in for deceleration. */
typedef struct {
    bool     enabled;
    int32_t  position_steps;
    int32_t  last_move_steps;
    uint32_t moves;
    uint32_t stops;
    uint32_t refused_busy;
    uint32_t polls_per_move;
    bool     stall;
    bool     fault_next;
    uint32_t fault_detail;
    /* state */
    bool     faulted;
    bool     moving;
    uint32_t polls_left;
    int32_t  pending_steps;
} truing_wheel_drive_fake_ctx_t;

void truing_wheel_drive_fake_init(truing_wheel_drive_if_t *self, truing_wheel_drive_fake_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_WHEEL_DRIVE_IF_H */
