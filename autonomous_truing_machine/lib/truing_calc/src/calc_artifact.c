/* Truing Calculation — REAL firmware implementation on a loaded artifact (Phase 1c, SPEC 8).
 *
 * The per-cycle solve is matrix-vector work only (SPEC 8.7): the pseudoinverses were
 * computed offline per layout and are applied ONLY to the row set they were computed for
 * (SPEC 8.7.1, verified through the admission's active row set). Implements the
 * dimensionless normalisation with sqrt(w) weighting and the T - s*T_norm tension
 * residual (SPEC 8.3.1), the two-part solve (SPEC 8.4: n_mt projection for FULL, no
 * projection under TENSION_ABSENT, d_cm as a per-spoke vector, asymmetric refused),
 * the row-count-normalised cost (SPEC 8.6) and the per-channel evaluation. */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing/row_layout.h"
#include "truing_calc/calc_if.h"

static truing_calc_artifact_ctx_t *ctx_of(truing_calc_if_t *self)
{
    return (truing_calc_artifact_ctx_t *)self->ctx;
}

static truing_side_t spoke_side(const truing_wheel_class_config_t *wheel, uint8_t i)
{
    /* Spokes alternate sides starting from the declared indexing origin (SPEC 6.5). */
    const truing_side_t origin = wheel->indexing_origin.side;
    if ((i % 2u) == 0u) {
        return origin;
    }
    return origin == TRUING_SIDE_A ? TRUING_SIDE_B : TRUING_SIDE_A;
}

/* ---- building blocks ---------------------------------------------------------------- */
bool truing_calc_residual(const truing_artifact_t *art, const truing_wheel_state_t *ws, const truing_row_mask_t *active,
                          truing_calc_residual_t *out)
{
    if (art == NULL || !art->loaded || ws == NULL || active == NULL || out == NULL) {
        return false;
    }
    if (ws->n_spokes != art->n_spokes || ws->n_rim_angles != art->n_rim_angles) {
        return false;
    }
    const uint8_t n = art->n_spokes, nra = art->n_rim_angles;
    truing_row_dims_t d;
    truing_row_dims_init(&d, n, nra);
    for (unsigned r = 0; r < TRUING_MAX_FULL_ROWS; ++r) {
        out->y_tilde[r] = NAN;
    }
    out->n_active_rows = 0u;
    out->s_scale = NAN;
    const float sq_wv = sqrtf(art->trust_radial), sq_wt = sqrtf(art->trust_tension);
    for (uint8_t k = 0; k < nra; ++k) {
        const truing_runout_measurement_t *m = truing_wheel_state_runout(ws, k);
        const bool ok = m != NULL && truing_status_is_solver_admissible(m->meta.status);
        if (truing_row_mask_test(active, truing_row_lateral(&d, k))) {
            if (!ok) return false;
            out->y_tilde[truing_row_lateral(&d, k)] = m->lateral_mm / art->tol_lateral_mm;
            out->n_active_rows++;
        }
        if (truing_row_mask_test(active, truing_row_radial(&d, k))) {
            if (!ok) return false;
            out->y_tilde[truing_row_radial(&d, k)] = sq_wv * m->radial_mm / art->tol_radial_mm;
            out->n_active_rows++;
        }
    }
    /* Tension rows: T - s*T_norm with s the LS projection of T onto the balanced distribution. */
    bool any_tension = false;
    float tt = 0.0f, tn = 0.0f;
    for (uint8_t i = 0; i < n; ++i) {
        if (!truing_row_mask_test(active, truing_row_tension(&d, i))) {
            continue;
        }
        const truing_tension_estimate_t *e = truing_wheel_state_spoke(ws, i);
        if (e == NULL || !truing_status_is_solver_admissible(e->meta.status)) {
            return false;
        }
        const float tnorm = art->T_target_present ? art->T_target_assumed[i] : 1.0f;
        tt += e->tension_n * tnorm;
        tn += tnorm * tnorm;
        any_tension = true;
    }
    if (any_tension) {
        out->s_scale = tt / tn;
        for (uint8_t i = 0; i < n; ++i) {
            if (!truing_row_mask_test(active, truing_row_tension(&d, i))) {
                continue;
            }
            const truing_tension_estimate_t *e = truing_wheel_state_spoke(ws, i);
            const float tnorm = art->T_target_present ? art->T_target_assumed[i] : 1.0f;
            out->y_tilde[truing_row_tension(&d, i)] = sq_wt * (e->tension_n - out->s_scale * tnorm) / art->tol_tension_n;
            out->n_active_rows++;
        }
    }
    return true;
}

bool truing_calc_ls_invert(const truing_artifact_t *art, truing_layout_id_t layout, const truing_calc_residual_t *res,
                           float d_ls[TRUING_MAX_SPOKES])
{
    const truing_artifact_layout_t *L = truing_artifact_layout(art, layout);
    if (L == NULL || res == NULL || d_ls == NULL) {
        return false;
    }
    /* SPEC 8.7.1: the pseudoinverse is applied to exactly its own row set. The residual must
     * carry a value for every row in the mask (and the caller guarantees no others are used). */
    float y[TRUING_MAX_FULL_ROWS];
    uint16_t c = 0u;
    for (uint16_t r = 0; r < TRUING_MAX_FULL_ROWS; ++r) {
        if (!truing_row_mask_test(&L->row_mask, r)) {
            continue;
        }
        if (!isfinite(res->y_tilde[r])) {
            return false;
        }
        y[c++] = res->y_tilde[r];
    }
    if (c != L->n_rows) {
        return false;
    }
    for (uint8_t i = 0; i < art->n_spokes; ++i) {
        float acc = 0.0f;
        for (uint16_t r = 0; r < L->n_rows; ++r) {
            acc += L->pinv[i][r] * y[r];
        }
        d_ls[i] = acc;
    }
    return true;
}

float truing_calc_cost(const truing_calc_residual_t *res)
{
    if (res == NULL || res->n_active_rows == 0u) {
        return NAN;
    }
    float sum = 0.0f;
    for (uint16_t r = 0; r < TRUING_MAX_FULL_ROWS; ++r) {
        if (isfinite(res->y_tilde[r])) {
            sum += res->y_tilde[r] * res->y_tilde[r];
        }
    }
    return sum / (float)res->n_active_rows;
}

/* ---- contract -------------------------------------------------------------------------- */
static bool art_model_available(truing_calc_if_t *self, const truing_wheel_class_config_t *wheel,
                                const truing_solver_config_t *solver, truing_reason_t *reason_out,
                                uint32_t *artifact_id_out, truing_fingerprint_t *fingerprint_out)
{
    (void)solver;
    truing_calc_artifact_ctx_t *c = ctx_of(self);
    const bool ok = c != NULL && c->artifact != NULL && c->artifact->loaded && wheel != NULL &&
                    truing_fingerprint_equals(&c->artifact->generating_fingerprint, &wheel->expected_influence_fingerprint);
    if (reason_out != NULL) {
        *reason_out = ok ? TRUING_REASON_NONE : TRUING_REASON_ARTIFACT_INVALID;
    }
    if (ok) {
        if (artifact_id_out != NULL) {
            *artifact_id_out = c->artifact->artifact_id;
        }
        if (fingerprint_out != NULL) {
            *fingerprint_out = c->artifact->generating_fingerprint;
        }
    }
    return ok;
}

static truing_layout_id_t art_select_layout(truing_calc_if_t *self, bool geometry_only, truing_reason_t *policy_reason_out)
{
    truing_calc_artifact_ctx_t *c = ctx_of(self);
    truing_layout_id_t layout = TRUING_LAYOUT_FULL;
    truing_reason_t reason = TRUING_REASON_NONE;
    if (c == NULL || c->artifact == NULL || !c->artifact->n_mt_identified) {
        layout = TRUING_LAYOUT_TENSION_ABSENT;               /* SPEC 8.4 Part 1 fallback: no projection possible */
        reason = TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE;
    } else if (geometry_only) {
        layout = TRUING_LAYOUT_TENSION_ABSENT;               /* explicit, traceable policy (SPEC 8.11) */
    }
    if (policy_reason_out != NULL) {
        *policy_reason_out = reason;
    }
    return layout;
}

static bool art_conditioning_ok(truing_calc_if_t *self, truing_layout_id_t layout, const truing_solver_config_t *solver,
                                truing_reason_t *reason_out)
{
    truing_calc_artifact_ctx_t *c = ctx_of(self);
    const truing_artifact_layout_t *L = c != NULL ? truing_artifact_layout(c->artifact, layout) : NULL;
    /* SPEC 8.11 R3: the firmware validates the RECORDED effective conditioning against limits. */
    const bool ok = L != NULL && solver != NULL && L->effective_condition_number <= solver->max_condition_number &&
                    (unsigned)L->effective_rank + (unsigned)L->expected_null_dim == c->artifact->n_spokes;
    if (reason_out != NULL) {
        *reason_out = ok ? TRUING_REASON_NONE : TRUING_REASON_ARTIFACT_INVALID;
    }
    return ok;
}

static bool art_solve(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                      const truing_solver_config_t *solver, truing_adjustment_plan_t *plan_out, truing_reason_t *reason_out)
{
    truing_calc_artifact_ctx_t *c = ctx_of(self);
    if (plan_out != NULL) {
        memset(plan_out, 0, sizeof(*plan_out));
    }
    if (c == NULL || c->artifact == NULL || !c->artifact->loaded || ws == NULL || admission == NULL || solver == NULL ||
        plan_out == NULL || !admission->admissible) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_ARTIFACT_INVALID;
        }
        return false;
    }
    const truing_artifact_t *art = c->artifact;
    const uint8_t n = art->n_spokes;
    c->solves++;
    truing_calc_residual_t res;
    float d_ls[TRUING_MAX_SPOKES];
    if (!truing_calc_residual(art, ws, &admission->active_row_set, &res) ||
        !truing_calc_ls_invert(art, admission->layout, &res, d_ls)) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_PARTIAL_WHEEL_STATE;
        }
        return false;
    }
    /* Part 1 (SPEC 8.4): FULL removes the n_mt component; TENSION_ABSENT applies no projection. */
    float d_shape[TRUING_MAX_SPOKES];
    if (admission->layout == TRUING_LAYOUT_FULL) {
        if (!art->n_mt_identified) {
            if (reason_out != NULL) {
                *reason_out = TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE;   /* must have been TENSION_ABSENT */
            }
            return false;
        }
        float proj = 0.0f;
        for (uint8_t i = 0; i < n; ++i) {
            proj += art->n_mt[i] * d_ls[i];
        }
        for (uint8_t i = 0; i < n; ++i) {
            d_shape[i] = -(d_ls[i] - proj * art->n_mt[i]);
        }
    } else {
        for (uint8_t i = 0; i < n; ++i) {
            d_shape[i] = -d_ls[i];
        }
    }
    /* Part 2 (SPEC 8.4): per-spoke VECTOR; symmetric formula only; asymmetric refused. */
    float d_cm[TRUING_MAX_SPOKES];
    memset(d_cm, 0, sizeof(d_cm));
    bool applied = false;
    truing_reason_t policy = admission->policy_reason;
    if (admission->layout == TRUING_LAYOUT_FULL && art->n_mt_identified && isfinite(res.s_scale)) {
        if (art->asymmetric || c->wheel == NULL || c->wheel->asymmetric) {
            policy = TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE;
        } else {
            float mean_t = 0.0f;
            for (uint8_t i = 0; i < n; ++i) {
                mean_t += truing_wheel_state_spoke(ws, i)->tension_n;
            }
            mean_t /= (float)n;
            for (uint8_t i = 0; i < n; ++i) {
                const float cc = spoke_side(c->wheel, i) == TRUING_SIDE_A ? art->c_side_a : art->c_side_b;
                d_cm[i] = (solver->target_tension_n - mean_t) / cc;
            }
            applied = true;
        }
    }
    plan_out->valid = true;
    plan_out->cycle_index = ws->cycle_index;
    plan_out->n_spokes = n;
    for (uint8_t i = 0; i < n; ++i) {
        plan_out->turns_rev[i] = d_shape[i] + d_cm[i];   /* Eq. 6, element-wise */
    }
    plan_out->artifact_id = art->artifact_id;
    plan_out->generating_fingerprint = art->generating_fingerprint;
    plan_out->active_layout = admission->layout;
    plan_out->active_row_set = admission->active_row_set;
    plan_out->n_active_rows = admission->n_active_rows;
    plan_out->n_valid_rows = admission->n_valid_rows;
    plan_out->n_suspect_rows = admission->n_suspect_rows;
    plan_out->tol_lateral_mm = art->tol_lateral_mm;
    plan_out->tol_radial_mm = art->tol_radial_mm;
    plan_out->tol_tension_n = art->tol_tension_n;
    plan_out->trust_radial = art->trust_radial;
    plan_out->trust_tension = art->trust_tension;
    plan_out->solver_version = 1u;
    plan_out->policy_reason = policy;
    plan_out->mean_tension_targeting_applied = applied;
    plan_out->cost_j = truing_calc_cost(&res);
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return true;
}

static bool art_predict_targets(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_adjustment_plan_t *plan,
                                float *predicted_lateral_mm_out, truing_reason_t *reason_out)
{
    truing_calc_artifact_ctx_t *c = ctx_of(self);
    if (c == NULL || c->artifact == NULL || !c->artifact->loaded || ws == NULL || plan == NULL || !plan->valid ||
        predicted_lateral_mm_out == NULL || ws->n_rim_angles != ws->n_spokes) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return false;
    }
    const truing_artifact_t *art = c->artifact;
    /* Eq. 2 for the lateral rows: u_hat = u + Phi_u d, evaluated at each spoke's own rim index. */
    for (uint8_t k = 0; k < art->n_rim_angles; ++k) {
        const truing_runout_measurement_t *m = truing_wheel_state_runout(ws, k);
        float u = (m != NULL && truing_status_is_solver_admissible(m->meta.status)) ? m->lateral_mm : NAN;
        for (uint8_t i = 0; i < art->n_spokes; ++i) {
            u += art->phi_u[k][i] * (plan->skipped[i] ? 0.0f : plan->turns_rev[i]);
        }
        predicted_lateral_mm_out[k] = u;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return true;
}

static bool art_verify(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                       const truing_solver_config_t *solver, truing_verification_result_t *out, truing_reason_t *reason_out)
{
    truing_calc_artifact_ctx_t *c = ctx_of(self);
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (c == NULL || c->artifact == NULL || !c->artifact->loaded || ws == NULL || admission == NULL || solver == NULL ||
        out == NULL || !admission->admissible) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_PARTIAL_WHEEL_STATE;
        }
        return false;
    }
    const truing_artifact_t *art = c->artifact;
    c->verifies++;
    truing_calc_residual_t res;
    if (!truing_calc_residual(art, ws, &admission->active_row_set, &res)) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_PARTIAL_WHEEL_STATE;
        }
        return false;
    }
    out->cycle_index = ws->cycle_index;
    out->layout = admission->layout;
    out->n_active_rows = res.n_active_rows;
    out->cost_j = truing_calc_cost(&res);
    out->max_lateral_mm = 0.0f;
    out->max_radial_mm = 0.0f;
    for (uint8_t k = 0; k < art->n_rim_angles; ++k) {
        const truing_runout_measurement_t *m = truing_wheel_state_runout(ws, k);
        if (m == NULL || !truing_status_is_solver_admissible(m->meta.status)) {
            continue;
        }
        if (fabsf(m->lateral_mm) > out->max_lateral_mm) out->max_lateral_mm = fabsf(m->lateral_mm);
        if (fabsf(m->radial_mm) > out->max_radial_mm) out->max_radial_mm = fabsf(m->radial_mm);
    }
    out->geometric_converged = out->max_lateral_mm <= solver->tol_lateral_mm && out->max_radial_mm <= solver->tol_radial_mm;
    out->tension_evaluated = admission->layout == TRUING_LAYOUT_FULL;
    out->non_uniformity_side_a = NAN;
    out->non_uniformity_side_b = NAN;
    out->mean_error_side_a_n = NAN;
    out->mean_error_side_b_n = NAN;
    if (out->tension_evaluated && c->wheel != NULL && art->T_target_present) {
        /* SPEC 8.6 / 11.2: per-side population CV and mean error against target * T_norm. */
        bool compliant = true;
        for (int side = 0; side < 2; ++side) {
            const truing_side_t s = side == 0 ? TRUING_SIDE_A : TRUING_SIDE_B;
            float sum = 0.0f, sumsq = 0.0f, tsum = 0.0f;
            unsigned cnt = 0u;
            for (uint8_t i = 0; i < art->n_spokes; ++i) {
                if (spoke_side(c->wheel, i) != s) continue;
                const truing_tension_estimate_t *e = truing_wheel_state_spoke(ws, i);
                if (e == NULL || !truing_status_is_solver_admissible(e->meta.status)) continue;
                sum += e->tension_n;
                sumsq += e->tension_n * e->tension_n;
                tsum += solver->target_tension_n * art->T_target_assumed[i];
                ++cnt;
            }
            float cv = NAN, me = NAN;
            if (cnt > 0u) {
                const float mean = sum / (float)cnt;
                float var = sumsq / (float)cnt - mean * mean;
                if (var < 0.0f) var = 0.0f;
                cv = mean != 0.0f ? sqrtf(var) / mean : NAN;
                me = fabsf(mean - tsum / (float)cnt);
            }
            if (s == TRUING_SIDE_A) { out->non_uniformity_side_a = cv; out->mean_error_side_a_n = me; }
            else { out->non_uniformity_side_b = cv; out->mean_error_side_b_n = me; }
            compliant = compliant && isfinite(cv) && cv <= solver->tol_tension_cv && me <= solver->tol_mean_tension_error_n;
        }
        out->tension_compliant = compliant;
    }
    out->reason = admission->policy_reason;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return true;
}

void truing_calc_artifact_init(truing_calc_if_t *self, truing_calc_artifact_ctx_t *ctx, const truing_artifact_t *artifact,
                               const truing_wheel_class_config_t *wheel)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->artifact = artifact;
    ctx->wheel = wheel;
    self->impl_name = "calc_artifact";
    self->source_impl = TRUING_SOURCE_REAL;
    self->model_available = art_model_available;
    self->select_layout = art_select_layout;
    self->conditioning_ok = art_conditioning_ok;
    self->solve = art_solve;
    self->predict_targets = art_predict_targets;
    self->verify = art_verify;
    self->ctx = ctx;
}
