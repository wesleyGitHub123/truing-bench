/**
 * @file navigation_synthetic.h
 * SYNTHETIC automated Wheel Navigation implementation (tests / simulation, SPEC §14.5).
 * Implementation header: this is the only place a wheel-drive type meets the
 * navigation capability. The orchestrator consumes navigation_if.h only.
 */
#ifndef TRUING_HAL_NAVIGATION_SYNTHETIC_H
#define TRUING_HAL_NAVIGATION_SYNTHETIC_H

#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_if.h"
#include "truing_hal/wheel_drive_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    truing_clock_if_t               clock;
    uint8_t                         n_spokes;
    uint8_t                         n_rim_angles;
    const truing_machine_profile_t *machine;
    truing_wheel_drive_if_t        *drive;                 /* optional actuator to command */
    float                           steps_per_wheel_rev;   /* CALIBRATION knob (test-provided); required if drive != NULL */
    uint8_t                         polls_per_move;        /* 0 = moves complete inside request() */
    uint8_t                         polls_remaining;
    float                           pending_delta_rad;
    float                           pending_target_rotation_rad;   /* R' the implementation believes it reaches */
    float                           true_rotation_rad;     /* what the wheel REALLY did (test oracle) */
    float                           scripted_slip_rad;     /* applied to the next move, then cleared */
    bool                            fault_next;            /* next move ends in FAULT */
    truing_nav_result_t             active;
    truing_wheel_position_t         position;
    uint32_t                        requests;
} truing_navigation_synthetic_ctx_t;

void truing_navigation_synthetic_init(truing_navigation_if_t *self, truing_navigation_synthetic_ctx_t *ctx,
                                      truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                      const truing_machine_profile_t *machine, truing_wheel_drive_if_t *drive,
                                      float steps_per_wheel_rev, uint8_t polls_per_move);
float truing_navigation_synthetic_true_rotation(const truing_navigation_synthetic_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_NAVIGATION_SYNTHETIC_H */
