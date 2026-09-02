/**
 * @file tension_model.h
 * Layer 4 (research repo lib/models, S4.2): the four candidate models behind one
 * interface, driven by the session-fixed tension-model profile (SPEC 9.2, 11.3.1).
 * No default model, no default parameter: a required field that is unestablished is
 * CALIBRATION_MISSING, never a nominal value.
 *
 *   IDEAL_STRING          T = 4 mu L^2 f1^2
 *   STIFFNESS_CORRECTED   T = 4 mu L^2 f1^2 - pi^2 EI / L^2
 *   HIGHER_MODE           two-mode inversion from f1, f2 recovering L_eff (bounded)
 *   EMPIRICAL             T = a f1^n
 */
#ifndef TRUING_DSP_TENSION_MODEL_H
#define TRUING_DSP_TENSION_MODEL_H

#include <stdbool.h>

#include "truing/config.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool            ok;
    truing_reason_t reason;          /* when !ok: CALIBRATION_MISSING, NO_F2_PARTNER, MODEL_REJECTED, FREQ_OUT_OF_RANGE */
    float           tension_n;
    float           l_eff_m;         /* HIGHER_MODE: recovered; others: the profile's L_eff (NAN if unused) */
    float           correction_n;    /* STIFFNESS_CORRECTED: the subtracted term */
} truing_tension_result_t;

/* f2_hz may be NAN when no partner was found. */
truing_tension_result_t truing_tension_model_apply(const truing_tension_model_profile_t *profile, float f1_hz, float f2_hz);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_TENSION_MODEL_H */
