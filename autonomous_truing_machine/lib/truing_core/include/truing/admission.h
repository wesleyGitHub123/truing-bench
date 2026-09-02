/**
 * @file admission.h
 * Solver admission rules R1, R2 and R4 with policy exclusion (SPEC §8.11).
 *
 *   available_rows  = rows whose measurement is valid or suspect
 *   active_row_set  = f(available_rows, solver policy)
 *
 * R1 layout match:  the active row set must EXACTLY equal a shipped layout's row mask.
 * R2 determinacy:   n_active_rows >= n_spokes.
 * R4 tension:       all present or all absent — partial loss is refused via R1.
 * R3 conditioning is artifact-dependent and belongs to the truing-calculation
 * implementation (SPEC §8.11 "where it is computed"); it is not evaluated here.
 *
 * Policy may exclude the whole tension channel even when every tension
 * measurement succeeded (n_mt unidentified, or geometry-only operation); the
 * measurements keep their true status — only the active row set changes.
 */
#ifndef TRUING_ADMISSION_H
#define TRUING_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/row_layout.h"
#include "truing/status.h"
#include "truing/wheel_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool               admissible;
    truing_layout_id_t layout;            /* the layout the active row set was matched against */
    truing_row_mask_t  available_rows;
    truing_row_mask_t  active_row_set;
    truing_row_mask_t  missing_rows;      /* row_mask(layout) minus available: what REMEASURE_GAPS must re-measure */
    uint16_t           n_active_rows;
    uint8_t            n_valid_rows;      /* SPEC §8.11: recorded on the AdjustmentPlan */
    uint8_t            n_suspect_rows;
    truing_reason_t    reason;            /* PARTIAL_WHEEL_STATE when refused, else NONE */
    truing_reason_t    policy_reason;     /* MEAN_TENSION_MODEL_UNAVAILABLE / NONE — recorded, never a failure */
    bool               tension_excluded_by_policy;
} truing_admission_t;

/**
 * Evaluate admission for the current wheel state.
 *
 * @param requested_layout  the policy-selected layout: FULL nominally, TENSION_ABSENT when policy
 *                          excludes the tension channel (n_mt unidentified, geometry-only).
 * @param policy_reason     the reason recorded for a policy exclusion (NONE for FULL).
 *
 * With FULL requested, a state whose tension rows are ALL absent legally selects
 * TENSION_ABSENT (SPEC §8.11 R4 table); SOME absent refuses via R1.
 */
void truing_admission_evaluate(const truing_wheel_state_t *ws, const truing_row_dims_t *dims,
                               truing_layout_id_t requested_layout, truing_reason_t policy_reason,
                               truing_admission_t *out);

/* Which spokes / rim indices the missing rows belong to (for REMEASURE_GAPS). */
bool truing_admission_missing_spoke(const truing_admission_t *a, const truing_row_dims_t *dims, uint8_t spoke_index);
bool truing_admission_missing_rim_index(const truing_admission_t *a, const truing_row_dims_t *dims, uint8_t rim_index);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_ADMISSION_H */
