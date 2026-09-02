/**
 * @file navigation_manual.h
 * MANUAL Wheel Navigation implementation (Stage 1: the human is the actuator).
 * Implementation header — the orchestrator consumes navigation_if.h only.
 */
#ifndef TRUING_HAL_NAVIGATION_MANUAL_H
#define TRUING_HAL_NAVIGATION_MANUAL_H

#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    truing_clock_if_t               clock;
    uint8_t                         n_spokes;
    uint8_t                         n_rim_angles;
    const truing_machine_profile_t *machine;
    truing_nav_result_t             active;
    truing_wheel_position_t         position;
    uint32_t                        requests;
    uint32_t                        confirmations;
} truing_navigation_manual_ctx_t;

void truing_navigation_manual_init(truing_navigation_if_t *self, truing_navigation_manual_ctx_t *ctx,
                                   truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                   const truing_machine_profile_t *machine);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_NAVIGATION_MANUAL_H */
