/* Truing Calculation SYNTHETIC double (SPEC §14.5 spirit, applied to the domain layer).
 *
 * Exists so the orchestrator's control flow can be host-tested before Phase 1c exists.
 * Its plan is a scripted rule (turns proportional to lateral runout, or a per-spoke
 * override) and its cost is the displacement-only part of the SPEC §8.6 normalised
 * residual. It performs NO influence-matrix solve, NO mean-tension targeting and NO
 * artifact checks; every number it produces is test data. */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing/row_layout.h"
#include "truing_calc/calc_if.h"

static truing_calc_synthetic_ctx_t *ctx_of(truing_calc_if_t *self)
{
    return (truing_calc_synthetic_ctx_t *)self->ctx;
}

static bool syn_model_available(truing_calc_if_t *self, const truing_wheel_class_config_t *wheel,
                                const truing_solver_config_t *solver, truing_reason_t *reason_out,
                                uint32_t *artifact_id_out, truing_fingerprint_t *fingerprint_out)
{
    (void)wheel;
    (void)solver;
    truing_calc_synthetic_ctx_t *c = ctx_of(self);
    if (c == NULL || !c->model_available) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_ARTIFACT_INVALID;
        }
        return false;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    if (artifact_id_out != NULL) {
        *artifact_id_out = c->artifact_id;
    }
    if (fingerprint_out != NULL) {
        *fingerprint_out = c->fingerprint;
    }
    return true;
}

static truing_layout_id_t syn_select_layout(truing_calc_if_t *self, bool geometry_only, truing_reason_t *policy_reason_out)
{
    truing_calc_synthetic_ctx_t *c = ctx_of(self);
    truing_reason_t reason = TRUING_REASON_NONE;
    truing_layout_id_t layout = TRUING_LAYOUT_FULL;
    if (c != NULL && !c->n_mt_identified) {
        layout = TRUING_LAYOUT_TENSION_ABSENT;              /* SPEC §8.4 Part 1 fallback */
        reason = TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE;
    } else if (geometry_only) {
        layout = TRUING_LAYOUT_TENSION_ABSENT;              /* explicit geometry-only policy */
        reason = TRUING_REASON_NONE;
    }
    if (policy_reason_out != NULL) {
        *policy_reason_out = reason;
    }
    return layout;
}

static bool syn_conditioning_ok(truing_calc_if_t *self, truing_layout_id_t layout, const truing_solver_config_t *solver,
                                truing_reason_t *reason_out)
{
    (void)solver;
    truing_calc_synthetic_ctx_t *c = ctx_of(self);
    const bool ok = c != NULL && (layout == TRUING_LAYOUT_FULL ? c->conditioning_ok_full : c->conditioning_ok_tension_absent);
    if (reason_out != NULL) {
        *reason_out = ok ? TRUING_REASON_NONE : TRUING_REASON_ARTIFACT_INVALID;
    }
    return ok;
}

/* Displacement-only normalised residual energy over the active rows (SPEC §8.6 shape). */
static float displacement_cost(const truing_wheel_state_t *ws, const truing_admission_t *adm, const truing_solver_config_t *cfg,
                               float *max_lat, float *max_rad)
{
    truing_row_dims_t d;
    truing_row_dims_init(&d, ws->n_spokes, ws->n_rim_angles);
    float sum = 0.0f;
    *max_lat = 0.0f;
    *max_rad = 0.0f;
    for (uint8_t k = 0; k < ws->n_rim_angles; ++k) {
        const truing_runout_measurement_t *m = truing_wheel_state_runout(ws, k);
        if (m == NULL || !truing_status_is_solver_admissible(m->meta.status)) {
            continue;
        }
        if (truing_row_mask_test(&adm->active_row_set, truing_row_lateral(&d, k))) {
            const float u = m->lateral_mm / cfg->tol_lateral_mm;
            sum += u * u;
            if (fabsf(m->lateral_mm) > *max_lat) {
                *max_lat = fabsf(m->lateral_mm);
            }
        }
        if (truing_row_mask_test(&adm->active_row_set, truing_row_radial(&d, k))) {
            const float v = m->radial_mm / cfg->tol_radial_mm;
            sum += cfg->trust_radial * v * v;
            if (fabsf(m->radial_mm) > *max_rad) {
                *max_rad = fabsf(m->radial_mm);
            }
        }
    }
    return adm->n_active_rows > 0u ? sum / (float)adm->n_active_rows : 0.0f;
}

static bool syn_solve(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                      const truing_solver_config_t *solver, truing_adjustment_plan_t *plan_out, truing_reason_t *reason_out)
{
    truing_calc_synthetic_ctx_t *c = ctx_of(self);
    if (plan_out == NULL || c == NULL || ws == NULL || admission == NULL || solver == NULL) {
        return false;
    }
    memset(plan_out, 0, sizeof(*plan_out));
    c->solves++;
    if (c->fail_solve || !admission->admissible) {
        c->fail_solve = false;
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_ARTIFACT_INVALID;
        }
        return false;
    }
    plan_out->valid = true;
    plan_out->cycle_index = ws->cycle_index;
    plan_out->n_spokes = ws->n_spokes;
    for (uint8_t i = 0; i < ws->n_spokes; ++i) {
        if (c->use_override) {
            plan_out->turns_rev[i] = c->turns_override[i];
        } else {
            const truing_runout_measurement_t *m = (ws->n_rim_angles == ws->n_spokes) ? truing_wheel_state_runout(ws, i) : NULL;
            const float lat = (m != NULL && truing_status_is_solver_admissible(m->meta.status)) ? m->lateral_mm : 0.0f;
            plan_out->turns_rev[i] = -c->turns_per_mm_lateral * lat;   /* synthetic rule, not a solve */
        }
    }
    plan_out->artifact_id = c->artifact_id;
    plan_out->generating_fingerprint = c->fingerprint;
    plan_out->active_layout = admission->layout;
    plan_out->active_row_set = admission->active_row_set;
    plan_out->n_active_rows = admission->n_active_rows;
    plan_out->n_valid_rows = admission->n_valid_rows;
    plan_out->n_suspect_rows = admission->n_suspect_rows;
    plan_out->tol_lateral_mm = solver->tol_lateral_mm;
    plan_out->tol_radial_mm = solver->tol_radial_mm;
    plan_out->tol_tension_n = solver->tol_tension_n;
    plan_out->trust_radial = solver->trust_radial;
    plan_out->trust_tension = solver->trust_tension;
    plan_out->solver_version = 0u;   /* synthetic: no solver version */
    plan_out->policy_reason = admission->policy_reason;
    plan_out->mean_tension_targeting_applied = false;   /* the double never targets mean tension */
    float ml, mr;
    plan_out->cost_j = displacement_cost(ws, admission, solver, &ml, &mr);
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return true;
}

static bool syn_predict_targets(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_adjustment_plan_t *plan,
                                float *predicted_lateral_mm_out, truing_reason_t *reason_out)
{
    truing_calc_synthetic_ctx_t *c = ctx_of(self);
    if (c == NULL || !c->provide_targets || ws == NULL || plan == NULL || predicted_lateral_mm_out == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return false;
    }
    for (uint8_t i = 0; i < ws->n_spokes; ++i) {
        predicted_lateral_mm_out[i] = 0.0f;   /* synthetic: the target of a corrected spoke is zero lateral */
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return true;
}

static void side_stats(const truing_wheel_state_t *ws, truing_side_t origin_side, truing_side_t side, const truing_solver_config_t *cfg,
                       float *cv, float *mean_err)
{
    /* Spokes alternate sides starting from the indexing origin (SPEC §6.5). */
    float sum = 0.0f, sumsq = 0.0f;
    unsigned n = 0u;
    for (uint8_t i = 0; i < ws->n_spokes; ++i) {
        const truing_side_t s = ((i % 2u) == 0u) ? origin_side : (origin_side == TRUING_SIDE_A ? TRUING_SIDE_B : TRUING_SIDE_A);
        if (s != side) {
            continue;
        }
        const truing_tension_estimate_t *e = truing_wheel_state_spoke(ws, i);
        if (e == NULL || !truing_status_is_solver_admissible(e->meta.status)) {
            continue;
        }
        sum += e->tension_n;
        sumsq += e->tension_n * e->tension_n;
        ++n;
    }
    if (n == 0u) {
        *cv = NAN;
        *mean_err = NAN;
        return;
    }
    const float mean = sum / (float)n;
    const float var = sumsq / (float)n - mean * mean;           /* population variance (SPEC §11.2) */
    *cv = mean != 0.0f ? sqrtf(var > 0.0f ? var : 0.0f) / mean : NAN;
    *mean_err = fabsf(mean - cfg->target_tension_n);            /* symmetric double: T_norm = 1 */
}

static bool syn_verify(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                       const truing_solver_config_t *solver, truing_verification_result_t *out, truing_reason_t *reason_out)
{
    truing_calc_synthetic_ctx_t *c = ctx_of(self);
    if (out == NULL || c == NULL || ws == NULL || admission == NULL || solver == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    c->verifies++;
    if (!admission->admissible) {
        out->reason = TRUING_REASON_PARTIAL_WHEEL_STATE;
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_PARTIAL_WHEEL_STATE;
        }
        return false;
    }
    out->cycle_index = ws->cycle_index;
    out->layout = admission->layout;
    out->n_active_rows = admission->n_active_rows;
    out->cost_j = displacement_cost(ws, admission, solver, &out->max_lateral_mm, &out->max_radial_mm);
    out->geometric_converged = out->max_lateral_mm <= solver->tol_lateral_mm && out->max_radial_mm <= solver->tol_radial_mm;
    out->tension_evaluated = admission->layout == TRUING_LAYOUT_FULL;
    if (out->tension_evaluated) {
        side_stats(ws, TRUING_SIDE_A, TRUING_SIDE_A, solver, &out->non_uniformity_side_a, &out->mean_error_side_a_n);
        side_stats(ws, TRUING_SIDE_A, TRUING_SIDE_B, solver, &out->non_uniformity_side_b, &out->mean_error_side_b_n);
        out->tension_compliant = isfinite(out->non_uniformity_side_a) && isfinite(out->non_uniformity_side_b) &&
                                 out->non_uniformity_side_a <= solver->tol_tension_cv &&
                                 out->non_uniformity_side_b <= solver->tol_tension_cv &&
                                 out->mean_error_side_a_n <= solver->tol_mean_tension_error_n &&
                                 out->mean_error_side_b_n <= solver->tol_mean_tension_error_n;
    } else {
        out->non_uniformity_side_a = NAN;
        out->non_uniformity_side_b = NAN;
        out->mean_error_side_a_n = NAN;
        out->mean_error_side_b_n = NAN;
    }
    out->reason = admission->policy_reason;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return true;
}

void truing_calc_synthetic_init(truing_calc_if_t *self, truing_calc_synthetic_ctx_t *ctx, uint32_t artifact_id)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->model_available = true;
    ctx->artifact_id = artifact_id;
    for (unsigned i = 0; i < TRUING_FINGERPRINT_BYTES; ++i) {
        ctx->fingerprint.bytes[i] = (uint8_t)(0xA0u + i);
    }
    ctx->fingerprint.set = true;
    ctx->n_mt_identified = true;
    ctx->conditioning_ok_full = true;
    ctx->conditioning_ok_tension_absent = true;
    ctx->turns_per_mm_lateral = 1.0f;
    ctx->provide_targets = true;
    self->impl_name = "calc_synthetic";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->model_available = syn_model_available;
    self->select_layout = syn_select_layout;
    self->conditioning_ok = syn_conditioning_ok;
    self->solve = syn_solve;
    self->predict_targets = syn_predict_targets;
    self->verify = syn_verify;
    self->ctx = ctx;
}
