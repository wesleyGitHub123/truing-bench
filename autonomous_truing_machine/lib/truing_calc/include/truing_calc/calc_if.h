/**
 * @file calc_if.h
 * Truing Calculation contract (SPEC §5.1 "Truing Calculation", §8).
 *
 * The orchestrator (Phase 1d) sequences the workflow; everything that touches the
 * influence-matrix artifact, the two-part solve, the cost function and the
 * per-channel evaluation sits behind this contract (Phase 1c, dependent on the
 * Phase 1b artifact). The orchestrator therefore never sees Φ, Φ†, weights or
 * layouts beyond the admission result it passes through.
 *
 * Two implementations exist here: a stub that reports no model (so a session
 * cannot start, SPEC §7.5 ABORT_NO_MODEL) and a SYNTHETIC double whose numbers
 * are test data, not a solver.
 */
#ifndef TRUING_CALC_IF_H
#define TRUING_CALC_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/admission.h"
#include "truing/config.h"
#include "truing/plan.h"
#include "truing/status.h"
#include "truing/wheel_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_calc_if truing_calc_if_t;

struct truing_calc_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    /* Is a valid, compatible influence artifact loaded for this configuration? (SPEC §8.7
     * integrity / fingerprint / shape checks live in the implementation.) On success the
     * artifact identity is returned for the session header and plans. */
    bool (*model_available)(truing_calc_if_t *self, const truing_wheel_class_config_t *wheel,
                            const truing_solver_config_t *solver, truing_reason_t *reason_out,
                            uint32_t *artifact_id_out, truing_fingerprint_t *fingerprint_out);
    /* Policy layout (SPEC §8.11): TENSION_ABSENT when n_mt is unidentified in the artifact
     * (MEAN_TENSION_MODEL_UNAVAILABLE) or geometry-only operation is requested; else FULL. */
    truing_layout_id_t (*select_layout)(truing_calc_if_t *self, bool geometry_only_requested, truing_reason_t *policy_reason_out);
    /* SPEC §8.11 R3 against the artifact's RECORDED per-layout effective rank / conditioning. */
    bool (*conditioning_ok)(truing_calc_if_t *self, truing_layout_id_t layout, const truing_solver_config_t *solver,
                            truing_reason_t *reason_out);
    /* Two-part solve (SPEC §8.4) on the ACTIVE row set of an admissible state. Fills the plan,
     * including cost_j of the state solved from (SPEC §8.6). */
    bool (*solve)(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                  const truing_solver_config_t *solver, truing_adjustment_plan_t *plan_out, truing_reason_t *reason_out);
    /* Predicted intermediate lateral state per spoke for the apply loop (SPEC §7.1 COMPUTE_TARGETS,
     * consumed by Stage 2 closed-loop actuation). Optional: NULL means not provided. */
    bool (*predict_targets)(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_adjustment_plan_t *plan,
                            float *predicted_lateral_mm_out, truing_reason_t *reason_out);
    /* Cost and per-channel evaluation of the CURRENT state over the active row set (SPEC §8.6). */
    bool (*verify)(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                   const truing_solver_config_t *solver, truing_verification_result_t *out, truing_reason_t *reason_out);
    void *ctx;
};

/* ---- Stub: no artifact (Phase 1c not built) ---------------------------------------- */
typedef struct {
    uint32_t calls;
} truing_calc_stub_ctx_t;

void truing_calc_stub_init(truing_calc_if_t *self, truing_calc_stub_ctx_t *ctx);

/* ---- SYNTHETIC double: scripted, deterministic, NOT a solver ------------------------ */
typedef struct {
    bool     model_available;
    uint32_t artifact_id;
    truing_fingerprint_t fingerprint;
    bool     n_mt_identified;             /* false -> TENSION_ABSENT policy, MEAN_TENSION_MODEL_UNAVAILABLE */
    bool     conditioning_ok_full;
    bool     conditioning_ok_tension_absent;
    float    turns_per_mm_lateral;        /* synthetic rule: turns[i] = -k * lateral(i); a TEST knob, not physics */
    float    turns_override[TRUING_MAX_SPOKES];   /* when `use_override`, plan turns come from here */
    bool     use_override;
    bool     fail_solve;                  /* next solve reports ARTIFACT_INVALID */
    bool     provide_targets;
    uint32_t solves;
    uint32_t verifies;
} truing_calc_synthetic_ctx_t;

void truing_calc_synthetic_init(truing_calc_if_t *self, truing_calc_synthetic_ctx_t *ctx, uint32_t artifact_id);

/* ---- REAL implementation on a loaded influence artifact (Phase 1c, SPEC 8) ---------- */
#include "truing/artifact.h"

typedef struct {
    const truing_artifact_t           *artifact;      /* loaded and checked by truing_artifact_load() */
    const truing_wheel_class_config_t *wheel;         /* side assignment for c_side and per-side stats */
    uint32_t                           solves;
    uint32_t                           verifies;
} truing_calc_artifact_ctx_t;

void truing_calc_artifact_init(truing_calc_if_t *self, truing_calc_artifact_ctx_t *ctx, const truing_artifact_t *artifact,
                               const truing_wheel_class_config_t *wheel);

/* Building blocks exposed for parity tests (SPEC 14.3.5). Row order: lateral, radial, tension. */
typedef struct {
    float    y_tilde[TRUING_MAX_FULL_ROWS];   /* full row order; NaN for rows not in the active set */
    float    s_scale;                         /* LS estimate of the current tension scale, NaN when no tension rows */
    uint16_t n_active_rows;
} truing_calc_residual_t;

/* Normalised residual over the active row set (SPEC 8.3.1). `u0`/`v0` are taken as zero (gauges tared). */
bool truing_calc_residual(const truing_artifact_t *art, const truing_wheel_state_t *ws, const truing_row_mask_t *active,
                          truing_calc_residual_t *out);
/* d_ls = Phi-dagger(L) y_tilde_active (Eq. 4). */
bool truing_calc_ls_invert(const truing_artifact_t *art, truing_layout_id_t layout, const truing_calc_residual_t *res,
                           float d_ls[TRUING_MAX_SPOKES]);
/* J(L) = ||y_tilde_active||^2 / n_active_rows (SPEC 8.6). */
float truing_calc_cost(const truing_calc_residual_t *res);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_CALC_IF_H */
