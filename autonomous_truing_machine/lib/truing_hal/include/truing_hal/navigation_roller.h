/**
 * @file navigation_roller.h
 * AUTOMATED Wheel Navigation over a rim-contact roller drive (SPEC §10A.6, Stage 2 shape),
 * open loop. Implementation header: the orchestrator consumes navigation_if.h only, and this
 * is one of the two places (with navigation_synthetic.h) where a wheel-drive type meets the
 * navigation capability.
 *
 * Reference: HYBRID. establish_reference() disables the drive so the wheel turns freely by hand
 * and answers PENDING_OPERATOR (TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION); the operator places
 * spoke 0 at the reference station and confirms; the confirmation re-enables the drive and
 * anchors the rotation state to the drive's step counter. Every later positioning is automated:
 * request() answers IN_MOTION and poll() resolves to DONE or FAULT. (SPEC §10A.2 allows the
 * reference step to be PENDING_OPERATOR; request() never is.)
 *
 * Position knowledge is DEAD RECKONING: the rotation state is derived from the pulses the
 * drive actually emitted since the anchor, divided by the calibration — never from the
 * commanded target, so step quantization and cut-short moves stay visible instead of hidden.
 * Nothing senses the wheel, so sensor_confirmed is always false and slip goes undetected until
 * the next reference (SPEC §10A.5). A move that overruns its deadline, a latched drive fault,
 * or a stop mid-move ends in FAULT / WHEEL_REFERENCE_LOST, and positioning is refused until the
 * reference is re-established.
 *
 * Provenance: as real as its drive (REAL over a physical driver, SYNTHETIC over the fake).
 */
#ifndef TRUING_HAL_NAVIGATION_ROLLER_H
#define TRUING_HAL_NAVIGATION_ROLLER_H

#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_if.h"
#include "truing_hal/step_ramp.h"
#include "truing_hal/wheel_drive_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Actuator-to-wheel calibration (SPEC §10A.4). Every field is MEASURED on the bench
 * (docs/ROLLER_BENCH.md) for one roller, one wheel and one driver configuration; none has a
 * default. An incomplete record refuses positioning with CALIBRATION_MISSING. */
typedef struct {
    /* SIGNED. Pulses per wheel revolution; the sign says which way the wheel turns: positive
     * pulses move the wheel toward +θ (CCW viewed from Side A, SPEC §6.4). 0 = unknown. */
    float       steps_per_wheel_rev;
    uint32_t    max_step_hz;      /* cruise rate the bench showed loses no steps under load */
    uint32_t    accel_steps_s2;   /* acceleration the bench showed loses no steps under load */
    uint16_t    microsteps;       /* driver setting the ratio was measured under (record) */
    const char *evidence;         /* bench-log reference the numbers come from; NULL = none */
} truing_roller_calibration_t;

bool truing_roller_calibration_valid(const truing_roller_calibration_t *c);

/* Move deadline (decision D4): the planned pulse-train duration times this factor, plus this
 * margin. A navigation POLICY bounding how long a hung or stalled drive can hold a session in
 * POSITION -- not a measured property of the mechanism. */
#define TRUING_NAV_ROLLER_DEADLINE_FACTOR    2.0f
#define TRUING_NAV_ROLLER_DEADLINE_MARGIN_MS 1000u

typedef struct {
    truing_clock_if_t               clock;
    uint8_t                         n_spokes;
    uint8_t                         n_rim_angles;
    const truing_machine_profile_t *machine;
    truing_wheel_drive_if_t        *drive;
    truing_roller_calibration_t     cal;
    float                           anchor_rotation_rad;
    int32_t                         anchor_steps;
    uint32_t                        move_deadline_ms;
    truing_nav_result_t             active;
    truing_wheel_position_t         position;
    uint32_t                        requests;
    uint32_t                        moves;
    uint32_t                        faults;
    uint32_t                        last_fault_detail;   /* from the drive, for logs */
} truing_navigation_roller_ctx_t;

/* `cal` is copied; it may be incomplete, in which case every positioning is refused. */
void truing_navigation_roller_init(truing_navigation_if_t *self, truing_navigation_roller_ctx_t *ctx,
                                   truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                   const truing_machine_profile_t *machine, truing_wheel_drive_if_t *drive,
                                   const truing_roller_calibration_t *cal);

/* Believed rotation state from the drive's step counter (NaN without a reference). */
float truing_navigation_roller_rotation_from_steps(const truing_navigation_roller_ctx_t *ctx, int32_t steps);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_NAVIGATION_ROLLER_H */
