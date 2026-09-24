/* Actuator-to-wheel calibration for the roller image (TRUING_ROLLER_NAV; decision D2, SPEC §10A.4).
 *
 * MEASURED values only, each traceable to a dated entry in docs/ROLLER_BENCH.md -- never a
 * placeholder to make the image run. It is EMPTY until the bench produces it: the image still
 * boots and the operator can still establish the reference, but every positioning is refused
 * with CALIBRATION_MISSING. That is the intended state of this file until R3/R4 pass.
 *
 * The numbers belong to ONE roller on ONE wheel under ONE driver configuration (contact
 * surface, tire pressure, preload and microsteps are part of the record in the bench log). A
 * different wheel or roller is a new measurement, not an edit to these values. */
#ifndef TRUING_ROLLER_CALIBRATION_H
#define TRUING_ROLLER_CALIBRATION_H

#include "truing_hal/navigation_roller.h"

static const truing_roller_calibration_t k_roller_calibration = {
    .steps_per_wheel_rev = 0.0f, /* R3: signed mean; + when + steps turn the wheel CCW viewed from Side A */
    .max_step_hz = 0u,           /* R2, confirmed by R4 with no lost steps */
    .accel_steps_s2 = 0u,        /* R2, confirmed by R4 */
    .microsteps = 0u,            /* R1 (verified); set and re-verified over the UART at boot */
    .evidence = NULL,            /* e.g. "ROLLER_BENCH R3 2026-09-28" */
};

#endif /* TRUING_ROLLER_CALIBRATION_H */
