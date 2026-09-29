/**
 * @file debug_code.h
 * Debug-channel command codes (SPEC §12.5): what TRUING_INTENT_DEBUG's `payload.debug.code`
 * selects. Development only, never in the demonstration script -- admission gates the intent
 * on debug_channel_enabled alone (operator_intent.c), never on session or state; this registry
 * is consulted only once an intent has already been admitted.
 *
 * A registry, not a dispatcher: adding a code here does not make it do anything by itself.
 * The orchestrator's intent dispatch (truing_orch_submit_intent(), orchestrator.c) is the only
 * place a code is interpreted, and it is the only place that may know what a code does.
 */
#ifndef TRUING_DEBUG_CODE_H
#define TRUING_DEBUG_CODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_DEBUG_CODE_UNSET = 0,
    /* Fire exactly one acoustic measurement on demand, with no orchestrator session, for
     * bench-driven campaign scripts. `arg` is the spoke_id, optionally packed with the two
     * campaign overrides below -- a bare spoke id (0..255) is the original shape and means
     * exactly what it always did. The estimate is discarded by the
     * dispatcher -- MEASURE_ONCE never becomes wheel state and never reaches the solver or
     * admission path (rule 4, docs/DEV_NOTES.md); its result is read back only through the
     * existing /debug/capture.json /.pcm evidence seam (SPEC §12.5), never a return value here. */
    TRUING_DEBUG_CODE_MEASURE_ONCE = 1,
    /* Bench drive (TRUING_BENCH_DRIVE images only): raw actuator control of the wheel drive, in
     * steps, so the roller can be moved by hand on the same board and command set as the mics.
     * No spoke, no station, no calibration: this is not navigation and never produces wheel state.
     * Every one is refused NOT_IMPLEMENTED when the image has no bench drive. Admission is unchanged
     * (debug channel on, READY, no session), and MEASURE_ONCE is refused while the drive moves. */
    TRUING_DEBUG_CODE_DRIVE_ENABLE = 2,   /* arg: 0 = coils off, 1 = on (clears a latched fault) */
    TRUING_DEBUG_CODE_DRIVE_MOVE = 3,     /* arg: signed relative steps, |arg| <= TRUING_DRIVE_MOVE_MAX_STEPS; returns at once */
    TRUING_DEBUG_CODE_DRIVE_STOP = 4,     /* arg ignored: controlled deceleration */
    TRUING_DEBUG_CODE_DRIVE_MOTION = 5,   /* arg: truing_drive_motion_args_t packed below */
    TRUING_DEBUG_CODE_DRIVE_ZERO = 6,     /* arg ignored: zero the step counter */
    TRUING_DEBUG_CODE__COUNT
} truing_debug_code_t;

/* A bench move this long is already dozens of motor revolutions; anything past it is a typo. */
#define TRUING_DRIVE_MOVE_MAX_STEPS 200000

/* DRIVE_MOTION's `arg`: the ramp the next moves use. Layout of the int32:
 *     bits  0..15  cruise step rate, steps/s (1..65535)
 *     bits 16..30  acceleration, in units of 100 steps/s^2 (1..32767)
 *     bit  31      must be zero */
typedef struct {
    uint16_t max_step_hz;
    uint32_t accel_steps_s2;   /* a multiple of 100 */
} truing_drive_motion_args_t;

int32_t truing_drive_motion_pack(const truing_drive_motion_args_t *a);
/* False -- and `out` untouched -- for a negative arg or a zero rate or acceleration. */
bool truing_drive_motion_unpack(int32_t arg, truing_drive_motion_args_t *out);

const char *truing_debug_code_str(truing_debug_code_t c);

/* MEASURE_ONCE's `arg`, unpacked. Layout of the int32:
 *
 *     bits  0..7   spoke_id
 *     bit   8      no_fire   -- run the whole capture and analysis but do not excite (a control:
 *                               SOLENOID_CAMPAIGN_PLAN.md B3.0). The actuator is never commanded.
 *     bits  9..15  reserved, must be zero
 *     bits 16..31  pulse_ms  -- one-shot override of this measurement's excitation pulse width,
 *                               in whole ms; 0 = use the excitation profile. B3.2's level sweep.
 *
 * Both overrides last for exactly the one measurement they ride in on. Neither touches the
 * excitation profile or its digest; the applied width is recorded in the capture as always.
 * Range-checking spoke_id against the wheel is the dispatcher's job -- it owns the wheel. */
typedef struct {
    uint8_t  spoke_id;
    bool     no_fire;
    uint16_t pulse_ms;
} truing_measure_once_args_t;

/* Pack for the wire. The inverse of unpack for every value unpack accepts. */
int32_t truing_measure_once_pack(const truing_measure_once_args_t *a);

/* False -- and `out` untouched -- for a negative arg, any reserved bit set, no_fire together with
 * a pulse override (contradictory), or a pulse override past TRUING_EXCITATION_PULSE_MAX_MS. */
bool truing_measure_once_unpack(int32_t arg, truing_measure_once_args_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DEBUG_CODE_H */
