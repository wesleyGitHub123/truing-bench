/* Truing Calculation STUB: no influence artifact is loadable, so no session can be
 * admitted (SPEC §7.5 "Influence matrix unavailable or invalid"). Every operation
 * reports its absence (P2). */
#include <stddef.h>
#include <string.h>

#include "truing_calc/calc_if.h"

static void bump(truing_calc_if_t *self)
{
    truing_calc_stub_ctx_t *ctx = (truing_calc_stub_ctx_t *)self->ctx;
    if (ctx != NULL) {
        ctx->calls++;
    }
}

static bool stub_model_available(truing_calc_if_t *self, const truing_wheel_class_config_t *wheel,
                                 const truing_solver_config_t *solver, truing_reason_t *reason_out,
                                 uint32_t *artifact_id_out, truing_fingerprint_t *fingerprint_out)
{
    (void)wheel;
    (void)solver;
    bump(self);
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    if (artifact_id_out != NULL) {
        *artifact_id_out = 0u;
    }
    if (fingerprint_out != NULL) {
        memset(fingerprint_out, 0, sizeof(*fingerprint_out));
    }
    return false;
}

static truing_layout_id_t stub_select_layout(truing_calc_if_t *self, bool geometry_only, truing_reason_t *policy_reason_out)
{
    (void)geometry_only;
    bump(self);
    if (policy_reason_out != NULL) {
        *policy_reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return TRUING_LAYOUT_NONE;
}

static bool stub_conditioning_ok(truing_calc_if_t *self, truing_layout_id_t layout, const truing_solver_config_t *solver,
                                 truing_reason_t *reason_out)
{
    (void)layout;
    (void)solver;
    bump(self);
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return false;
}

static bool stub_solve(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                       const truing_solver_config_t *solver, truing_adjustment_plan_t *plan_out, truing_reason_t *reason_out)
{
    (void)ws;
    (void)admission;
    (void)solver;
    bump(self);
    if (plan_out != NULL) {
        memset(plan_out, 0, sizeof(*plan_out));   /* valid = false: no plan, never a zero plan */
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return false;
}

static bool stub_verify(truing_calc_if_t *self, const truing_wheel_state_t *ws, const truing_admission_t *admission,
                        const truing_solver_config_t *solver, truing_verification_result_t *out, truing_reason_t *reason_out)
{
    (void)ws;
    (void)admission;
    (void)solver;
    bump(self);
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return false;
}

void truing_calc_stub_init(truing_calc_if_t *self, truing_calc_stub_ctx_t *ctx)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    self->impl_name = "calc_stub";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->model_available = stub_model_available;
    self->select_layout = stub_select_layout;
    self->conditioning_ok = stub_conditioning_ok;
    self->solve = stub_solve;
    self->predict_targets = NULL;
    self->verify = stub_verify;
    self->ctx = ctx;
}
