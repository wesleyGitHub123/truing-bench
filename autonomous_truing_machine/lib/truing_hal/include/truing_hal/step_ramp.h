/**
 * @file step_ramp.h
 * Trapezoidal step-pulse timing for a STEP/DIR stepper driver, in ACTUATOR coordinates only
 * (SPEC §10A.1). Pure and framework-free, so the pulse sequence is host-testable; the target
 * side supplies only a timer that emits one pulse per call.
 *
 * The algorithm is the integer form of D. Austin's real-time speed-profile recurrence (as in
 * Atmel AVR446): c_n = c_{n-1} - 2·c_{n-1} / (4n + 1), with the remainder carried so rounding
 * does not accumulate. truing_ramp_step() is integer-only on purpose: it runs in a timer ISR,
 * and the ESP32-S3 does not preserve FPU state for interrupt handlers.
 *
 * Contract, per move:
 *   truing_ramp_plan()   task context; arm the timer for `delay` ticks
 *   on each alarm        emit one pulse, then  next = truing_ramp_step();
 *                        next == 0  -> that pulse was the last one: stop the timer
 *                        otherwise  -> re-arm for `next` ticks
 *   truing_ramp_request_stop()  any context: the move ends after the fewest further pulses
 *                        that decelerate it (a controlled stop, SPEC §10A.7)
 *
 * The profile values (maximum step rate, acceleration) are NOT defaults: they come from the
 * bench characterization of the actual motor, roller and load (docs/ROLLER_BENCH.md).
 */
#ifndef TRUING_HAL_STEP_RAMP_H
#define TRUING_HAL_STEP_RAMP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tick_hz;          /* timer resolution the delays are expressed in */
    uint32_t max_step_hz;      /* cruise step rate */
    uint32_t accel_steps_s2;   /* acceleration and deceleration, steps/s² */
} truing_ramp_profile_t;

typedef enum {
    TRUING_RAMP_IDLE = 0,
    TRUING_RAMP_ACCEL,
    TRUING_RAMP_RUN,
    TRUING_RAMP_DECEL,
} truing_ramp_phase_t;

typedef struct {
    truing_ramp_phase_t phase;
    uint32_t total;              /* pulses in this move (may shrink on a stop request) */
    uint32_t done;               /* pulses emitted so far */
    uint32_t decel_start;        /* pulse count at which deceleration begins */
    int32_t  decel_val;          /* -(pulses needed to decelerate) */
    int32_t  accel_count;        /* recurrence index n; negative while decelerating */
    int32_t  delay;              /* ticks until the pending pulse */
    int32_t  min_delay;          /* ticks between pulses at cruise */
    int32_t  last_accel_delay;   /* delay when cruise began; first delay of deceleration */
    int32_t  rest;               /* remainder carried by the recurrence */
    int32_t  accel_steps;        /* pulses spent accelerating before cruise */
    volatile bool stop_req;
} truing_ramp_t;

/* Plan a move of `steps` pulses (the direction is the caller's). Returns false, leaving the
 * ramp IDLE, for zero steps or a profile that cannot be timed. On success r->delay is the
 * number of ticks before the first pulse. */
bool truing_ramp_plan(truing_ramp_t *r, const truing_ramp_profile_t *p, uint32_t steps);

/* Call once per emitted pulse (ISR-safe, integer only). Returns the ticks until the next
 * pulse, or 0 when the pulse just emitted completed the move. */
uint32_t truing_ramp_step(truing_ramp_t *r);

/* Controlled stop: applied by the next truing_ramp_step(). A move already decelerating, or
 * one that would end sooner anyway, is left as planned. */
void truing_ramp_request_stop(truing_ramp_t *r);

bool truing_ramp_active(const truing_ramp_t *r);

/* Idealised duration of a move of `steps` pulses under `p`, in seconds (task context). A
 * planning estimate for deadlines, not a measurement. Negative for an invalid profile. */
float truing_ramp_duration_s(const truing_ramp_profile_t *p, uint32_t steps);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_STEP_RAMP_H */
