/**
 * @file plan.h
 * AdjustmentPlan and VerificationResult data types (SPEC §6.3) plus the pure
 * plan checks the orchestrator applies: the safety bound
 * (ABORT_UNSAFE_ADJUSTMENT, SPEC §7.5) and the adjustment deadband (SPEC §11.2).
 */
#ifndef TRUING_PLAN_H
#define TRUING_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing/row_layout.h"
#include "truing/status.h"
#include "truing/wheel_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;                              /* false until a solve produced it */
    uint8_t  cycle_index;
    uint8_t  n_spokes;
    float    turns_rev[TRUING_MAX_SPOKES];       /* per-spoke required rotation; positive = tightening (SPEC §6.4) */
    bool     skipped[TRUING_MAX_SPOKES];         /* below adjustment_deadband while locally within tolerance */
    uint32_t artifact_id;                        /* SPEC §6.3: artifact ID and generating fingerprint */
    truing_fingerprint_t generating_fingerprint;
    truing_layout_id_t active_layout;            /* SPEC §6.3: active layout and active_row_set */
    truing_row_mask_t  active_row_set;
    uint16_t n_active_rows;
    uint8_t  n_valid_rows;                       /* SPEC §8.11: valid vs suspect contributing rows */
    uint8_t  n_suspect_rows;
    float    tol_lateral_mm;                     /* weights and tolerances used (SPEC §6.3) */
    float    tol_radial_mm;
    float    tol_tension_n;
    float    trust_radial;
    float    trust_tension;
    uint16_t solver_version;
    truing_reason_t policy_reason;               /* e.g. MEAN_TENSION_MODEL_UNAVAILABLE, or NONE */
    bool     mean_tension_targeting_applied;     /* Part 2 applied (SPEC §8.4) */
    float    cost_j;                             /* J of the state the plan was solved from (SPEC §8.6) */
} truing_adjustment_plan_t;

typedef struct {
    uint8_t  cycle_index;
    truing_layout_id_t layout;                   /* J is comparable only within one layout (SPEC §8.6.1) */
    uint16_t n_active_rows;
    float    cost_j;                             /* scalar cost, row-count normalised (SPEC §6.3, §8.6) */
    float    max_lateral_mm;
    float    max_radial_mm;
    bool     geometric_converged;                /* max|u| <= tol_lateral AND max|v| <= tol_radial */
    bool     tension_evaluated;                  /* tension rows were in the active set */
    bool     tension_compliant;                  /* numeric per-side CV and mean-error test only;
                                                    verification-grade evidence is judged by the caller (SPEC §8.6.3) */
    float    non_uniformity_side_a;              /* population CV per side (SPEC §11.2) */
    float    non_uniformity_side_b;
    float    mean_error_side_a_n;
    float    mean_error_side_b_n;
    truing_reason_t reason;
} truing_verification_result_t;

/* Largest |turns_rev| over spokes not marked skipped; 0 for an invalid plan. */
float truing_plan_max_abs_turn(const truing_adjustment_plan_t *plan);

/* SPEC §7.5 / §11.2: any requested rotation beyond max_adjustment_revolutions is unsafe.
 * Returns true when unsafe; `spoke_out` receives the first offending spoke. */
bool truing_plan_is_unsafe(const truing_adjustment_plan_t *plan, const truing_solver_config_t *cfg, uint8_t *spoke_out);

/* SPEC §11.2 adjustment_deadband: an adjustment too small to perform reliably is skipped
 * when the wheel state at that spoke (its rim-index runout) is already within tolerance.
 * Requires n_rim_angles == n_spokes (Stage 1 policy, SPEC §8.9). Returns the number skipped. */
uint8_t truing_plan_apply_deadband(truing_adjustment_plan_t *plan, const truing_solver_config_t *cfg,
                                   const truing_wheel_state_t *ws);

/* Number of spokes that still require an adjustment (valid, not skipped, non-zero). */
uint8_t truing_plan_count_pending(const truing_adjustment_plan_t *plan);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_PLAN_H */
