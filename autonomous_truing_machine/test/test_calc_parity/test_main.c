/* Host/firmware parity of the REAL truing calculation on the golden sym32 artifact (SPEC 14.3.5),
 * the contract behaviour around the unidentified common mode, and the complete workflow driven by
 * the real calculation against a wheel that responds through the same influence model. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing/admission.h"
#include "truing/artifact.h"
#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixture_sym32_parity.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/auto_operator.h"
#include "truing_orch/orchestrator.h"

/* Tolerances for float32 arithmetic against the float64 host reference (recorded in docs). */
#define PARITY_REL 1e-4f
#define PARITY_ABS_REV 1e-6f

static truing_artifact_t g_art;
static truing_wheel_class_config_t g_wheel;
static truing_solver_config_t g_solver;
static truing_wheel_state_t g_ws;
static truing_calc_if_t g_calc;
static truing_calc_artifact_ctx_t g_cctx;
static float g_worst_rel;   /* largest relative deviation seen on any inverted vector */

void setUp(void)
{
    truing_fixture_wheel_class_sym32(&g_wheel);
    truing_fixture_solver_config(&g_solver, 32u);
    const char *detail = NULL;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_OK, truing_artifact_load(fixture_sym32_artifact_blob, fixture_sym32_artifact_blob_len,
                                                              &g_wheel, &g_solver, &g_art, &detail));
    truing_calc_artifact_init(&g_calc, &g_cctx, &g_art, &g_wheel);
}
void tearDown(void) {}

static void build_state(const float *u, const float *v, const float *T)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 1u));
    for (uint8_t k = 0; k < 32u; ++k) {
        truing_runout_measurement_t m;
        test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, k), u[k], v[k]);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, k, &m, NULL));
    }
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_tension_estimate_t e;
        test_make_valid_estimate(&e, 1u, T[i], 480.0f);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, NULL));
    }
}

static void expect_vec(const float *got, const float *want, uint8_t n, const char *what)
{
    for (uint8_t i = 0; i < n; ++i) {
        const float tol = fmaxf(PARITY_ABS_REV, PARITY_REL * fabsf(want[i]));
        if (fabsf(want[i]) > 1e-3f) {
            const float rel = fabsf(got[i] - want[i]) / fabsf(want[i]);
            if (rel > g_worst_rel) g_worst_rel = rel;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "%s[%u] want %.7g got %.7g", what, (unsigned)i, (double)want[i], (double)got[i]);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(tol, want[i], got[i], msg);
    }
}

static void test_residual_cost_and_ls_inversion_match_host_per_layout(void)
{
    truing_row_dims_t d;
    truing_row_dims_init(&d, 32u, 32u);
    g_worst_rel = 0.0f;
    for (unsigned ci = 0; ci < FIXTURE_SYM32_PARITY_N_CASES; ++ci) {
        const fixture_sym32_parity_case_t *c = &fixture_sym32_parity_cases[ci];
        build_state(c->u_mm, c->v_mm, c->T_n);
        for (unsigned li = 0; li < 2u; ++li) {
            const fixture_sym32_parity_layout_case_t *L = &c->layouts[li];
            const truing_layout_id_t layout = L->layout == 1 ? TRUING_LAYOUT_FULL : TRUING_LAYOUT_TENSION_ABSENT;
            truing_row_mask_t mask;
            TEST_ASSERT_TRUE(truing_row_mask_for_layout(layout, &d, &mask));
            truing_calc_residual_t res;
            TEST_ASSERT_TRUE(truing_calc_residual(&g_art, &g_ws, &mask, &res));
            TEST_ASSERT_EQUAL_UINT16((uint16_t)L->n_active_rows, res.n_active_rows);
            TEST_ASSERT_FLOAT_WITHIN(1e-5f * L->cost_j, L->cost_j, truing_calc_cost(&res));
            if (layout == TRUING_LAYOUT_FULL) {
                TEST_ASSERT_FLOAT_WITHIN(1e-6f * L->s_scale, L->s_scale, res.s_scale);
            } else {
                TEST_ASSERT_TRUE(isnan(res.s_scale));
            }
            /* The LS inversion itself is layout-pure linear algebra and must agree even for the FULL
             * layout the host then REFUSED to turn into a plan (n_mt unidentified). */
            float d_ls[TRUING_MAX_SPOKES];
            TEST_ASSERT_TRUE(truing_calc_ls_invert(&g_art, layout, &res, d_ls));
            expect_vec(d_ls, L->d_ls, 32u, layout == TRUING_LAYOUT_FULL ? "FULL d_ls" : "TA d_ls");
        }
    }
    printf("parity: worst relative deviation of d_ls over %u cases x 2 layouts = %.3g (limit %.1g)\n",
           (unsigned)FIXTURE_SYM32_PARITY_N_CASES, (double)g_worst_rel, (double)PARITY_REL);
}

static void test_contract_solve_matches_host_under_tension_absent(void)
{
    truing_row_dims_t d;
    truing_row_dims_init(&d, 32u, 32u);
    for (unsigned ci = 0; ci < FIXTURE_SYM32_PARITY_N_CASES; ++ci) {
        const fixture_sym32_parity_case_t *c = &fixture_sym32_parity_cases[ci];
        const fixture_sym32_parity_layout_case_t *L = &c->layouts[1];
        TEST_ASSERT_EQUAL_INT(2, L->layout);
        TEST_ASSERT_EQUAL_INT(0, L->refused);
        build_state(c->u_mm, c->v_mm, c->T_n);
        truing_reason_t policy = TRUING_REASON_NONE;
        const truing_layout_id_t chosen = g_calc.select_layout(&g_calc, false, &policy);
        TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, chosen);
        TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, policy);
        truing_admission_t adm;
        truing_admission_evaluate(&g_ws, &d, chosen, policy, &adm);
        TEST_ASSERT_TRUE(adm.admissible);
        TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, adm.layout);
        TEST_ASSERT_EQUAL_UINT16(64u, adm.n_active_rows);
        truing_adjustment_plan_t plan;
        truing_reason_t why = TRUING_REASON_NONE;
        TEST_ASSERT_TRUE(g_calc.solve(&g_calc, &g_ws, &adm, &g_solver, &plan, &why));
        TEST_ASSERT_TRUE(plan.valid);
        expect_vec(plan.turns_rev, L->d_adj, 32u, "plan turns");
        TEST_ASSERT_FLOAT_WITHIN(1e-5f * L->cost_j, L->cost_j, plan.cost_j);
        TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, plan.active_layout);
        TEST_ASSERT_TRUE(truing_row_mask_equals(&adm.active_row_set, &plan.active_row_set));
        TEST_ASSERT_EQUAL_UINT16(64u, plan.n_active_rows);
        TEST_ASSERT_EQUAL_UINT8(64u, plan.n_valid_rows);
        TEST_ASSERT_EQUAL_UINT8(0u, plan.n_suspect_rows);
        TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, plan.policy_reason);
        TEST_ASSERT_FALSE(plan.mean_tension_targeting_applied);
        TEST_ASSERT_EQUAL_UINT32(1u, plan.artifact_id);
        TEST_ASSERT_TRUE(truing_fingerprint_equals(&plan.generating_fingerprint, &g_wheel.expected_influence_fingerprint));
        TEST_ASSERT_EQUAL_FLOAT(g_solver.tol_lateral_mm, plan.tol_lateral_mm);
        TEST_ASSERT_EQUAL_FLOAT(g_solver.trust_radial, plan.trust_radial);
        /* d_cm is identically zero here, so turns == d_shape == -d_ls: the host says the same. */
        expect_vec(plan.turns_rev, L->d_shape, 32u, "d_shape");
    }
}

static void test_contract_refuses_full_when_n_mt_is_unidentified(void)
{
    const fixture_sym32_parity_case_t *c = &fixture_sym32_parity_cases[0];
    TEST_ASSERT_EQUAL_INT(1, c->layouts[0].refused);     /* host: MEAN_TENSION_MODEL_UNAVAILABLE */
    build_state(c->u_mm, c->v_mm, c->T_n);
    truing_reason_t why = TRUING_REASON_NONE;
    uint32_t id = 0u;
    truing_fingerprint_t fp;
    TEST_ASSERT_TRUE(g_calc.model_available(&g_calc, &g_wheel, &g_solver, &why, &id, &fp));
    TEST_ASSERT_EQUAL_UINT32(1u, id);
    TEST_ASSERT_TRUE(truing_fingerprint_equals(&fp, &g_wheel.expected_influence_fingerprint));
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_REAL, g_calc.source_impl);
    /* geometry-only requested or not: with n_mt unidentified the layout is TENSION_ABSENT either way */
    truing_reason_t policy = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, g_calc.select_layout(&g_calc, true, &policy));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, policy);
    TEST_ASSERT_TRUE(g_calc.conditioning_ok(&g_calc, TRUING_LAYOUT_FULL, &g_solver, &why));
    TEST_ASSERT_TRUE(g_calc.conditioning_ok(&g_calc, TRUING_LAYOUT_TENSION_ABSENT, &g_solver, &why));
    truing_solver_config_t tight = g_solver;
    tight.max_condition_number = 1.5f;
    TEST_ASSERT_FALSE(g_calc.conditioning_ok(&g_calc, TRUING_LAYOUT_FULL, &tight, &why));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_ARTIFACT_INVALID, why);
    /* A FULL admission handed to solve anyway is refused with the same reason the host recorded. */
    truing_row_dims_t d;
    truing_row_dims_init(&d, 32u, 32u);
    truing_admission_t adm;
    truing_admission_evaluate(&g_ws, &d, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &adm);
    TEST_ASSERT_TRUE(adm.admissible);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, adm.layout);
    truing_adjustment_plan_t plan;
    TEST_ASSERT_FALSE(g_calc.solve(&g_calc, &g_ws, &adm, &g_solver, &plan, &why));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, why);
    TEST_ASSERT_FALSE(plan.valid);
}

static void test_verify_reports_cost_geometry_and_no_tension_under_tension_absent(void)
{
    const fixture_sym32_parity_case_t *c = &fixture_sym32_parity_cases[0];
    build_state(c->u_mm, c->v_mm, c->T_n);
    truing_row_dims_t d;
    truing_row_dims_init(&d, 32u, 32u);
    truing_admission_t adm;
    truing_admission_evaluate(&g_ws, &d, TRUING_LAYOUT_TENSION_ABSENT, TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, &adm);
    truing_verification_result_t v;
    truing_reason_t why = TRUING_REASON_NONE;
    TEST_ASSERT_TRUE(g_calc.verify(&g_calc, &g_ws, &adm, &g_solver, &v, &why));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f * c->layouts[1].cost_j, c->layouts[1].cost_j, v.cost_j);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, v.layout);
    TEST_ASSERT_FALSE(v.geometric_converged);                 /* max|u| = 0.223 mm > 0.1 mm */
    TEST_ASSERT_FALSE(v.tension_evaluated);
    TEST_ASSERT_TRUE(isnan(v.non_uniformity_side_a));
    float zeros[32];
    memset(zeros, 0, sizeof(zeros));
    build_state(zeros, zeros, c->T_n);
    truing_admission_evaluate(&g_ws, &d, TRUING_LAYOUT_TENSION_ABSENT, TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, &adm);
    TEST_ASSERT_TRUE(g_calc.verify(&g_calc, &g_ws, &adm, &g_solver, &v, &why));
    TEST_ASSERT_TRUE(v.geometric_converged);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.cost_j);
}

/* Exact linear world: u = Phi_u d, v = Phi_v d. The TENSION_ABSENT inversion has full column rank on
 * this artifact, so the plan must be -d and the predicted post-adjustment lateral state ~0. */
static void exact_state_from(const float *d_rev, float *u, float *v)
{
    for (uint8_t k = 0; k < 32u; ++k) {
        float uu = 0.0f, vv = 0.0f;
        for (uint8_t i = 0; i < 32u; ++i) {
            uu += g_art.phi_u[k][i] * d_rev[i];
            vv += g_art.phi_v[k][i] * d_rev[i];
        }
        u[k] = uu;
        v[k] = vv;
    }
}

static void test_exact_linear_state_is_recovered_and_predicted_to_zero(void)
{
    const fixture_sym32_parity_case_t *c = &fixture_sym32_parity_cases[0];
    float u[32], v[32];
    exact_state_from(c->d_applied, u, v);
    build_state(u, v, c->T_n);
    truing_row_dims_t d;
    truing_row_dims_init(&d, 32u, 32u);
    truing_admission_t adm;
    truing_admission_evaluate(&g_ws, &d, TRUING_LAYOUT_TENSION_ABSENT, TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, &adm);
    truing_adjustment_plan_t plan;
    truing_reason_t why;
    TEST_ASSERT_TRUE(g_calc.solve(&g_calc, &g_ws, &adm, &g_solver, &plan, &why));
    for (uint8_t i = 0; i < 32u; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, -c->d_applied[i], plan.turns_rev[i]);
    }
    float predicted[32];
    TEST_ASSERT_TRUE(g_calc.predict_targets(&g_calc, &g_ws, &plan, predicted, &why));
    for (uint8_t k = 0; k < 32u; ++k) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, predicted[k]);
    }
}

/* ---- the complete workflow on the real calculation --------------------------------- */
typedef struct {
    uint32_t events;
    uint32_t transitions;
    truing_terminal_result_t last_terminal;
} sink_ctx_t;

static bool sink_emit(truing_telemetry_if_t *self, const truing_telemetry_event_t *ev)
{
    sink_ctx_t *c = (sink_ctx_t *)self->ctx;
    c->events++;
    if (ev->kind == TRUING_EVT_STATE_TRANSITION) c->transitions++;
    if (ev->kind == TRUING_EVT_TERMINAL_RESULT) c->last_terminal = ev->u.terminal;
    return true;
}

typedef struct {
    truing_chain_profile_t chain;
    truing_tension_model_profile_t tmodel;
    truing_machine_profile_t machine;
    truing_fake_clock_t fc;
    truing_clock_if_t clock;
    truing_acoustic_if_t acoustic;
    truing_acoustic_synthetic_ctx_t actx;
    truing_runout_if_t runout;
    truing_runout_manual_ctx_t rctx;
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_telemetry_if_t sink;
    sink_ctx_t sctx;
    truing_auto_operator_t op;
    truing_orchestrator_t orch;
} rig_t;

static rig_t g_rig;

/* The wheel answers an adjustment through the SAME influence model the solver uses. */
static void model_response(void *user, truing_auto_wheel_model_t *wheel, uint8_t spoke, float turns_rev)
{
    const truing_artifact_t *art = (const truing_artifact_t *)user;
    for (uint8_t k = 0; k < art->n_rim_angles; ++k) {
        wheel->lateral_mm[k] += art->phi_u[k][spoke] * turns_rev;
        wheel->radial_mm[k] += art->phi_v[k][spoke] * turns_rev;
    }
}

static void test_workflow_converges_on_the_real_model_geometry_only(void)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof(*r));
    truing_fixture_chain_profile_inmp441(&r->chain);
    truing_fixture_tension_model_profile_complete(&r->tmodel);
    truing_fixture_machine_profile(&r->machine);
    truing_fake_clock_init(&r->fc, &r->clock, 1000u);
    truing_acoustic_synthetic_init(&r->acoustic, &r->actx, r->clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    r->actx.snr_db = 30.0f;
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_acoustic_synthetic_set_spoke(&r->actx, i, 1000.0f, 480.0f);
    }
    truing_runout_manual_init(&r->runout, &r->rctx, r->clock);
    truing_navigation_manual_init(&r->nav, &r->nctx, r->clock, 32u, 32u, &r->machine);
    r->sink.impl_name = "test_sink";
    r->sink.source_impl = TRUING_SOURCE_SYNTHETIC;
    r->sink.emit = sink_emit;
    r->sink.ctx = &r->sctx;
    truing_auto_operator_init(&r->op);
    float u[32], v[32];
    exact_state_from(fixture_sym32_parity_cases[3].d_applied, u, v);   /* the largest starting error, 0.274 mm */
    truing_auto_operator_set_wheel(&r->op, 32u, u, v, 0.0f);
    r->op.response_fn = model_response;
    r->op.user = &g_art;

    truing_orch_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.wheel = &g_wheel;
    deps.solver = &g_solver;
    deps.chain = &r->chain;
    deps.tension_model = &r->tmodel;
    deps.machine = &r->machine;
    deps.acoustic = &r->acoustic;
    deps.runout = &r->runout;
    deps.navigation = &r->nav;
    deps.calc = &g_calc;
    deps.telemetry = &r->sink;
    deps.clock = r->clock;
    deps.firmware_version = "test";
    TEST_ASSERT_TRUE(truing_orch_init(&r->orch, &deps));
    uint32_t steps = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_IDLE, truing_orch_run_auto(&r->orch, &r->op, 50u, &steps));
    truing_intent_t start;
    memset(&start, 0, sizeof(start));
    start.type = TRUING_INTENT_START_TRUING;
    truing_reason_t why = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&r->orch, &start, &why));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, truing_orch_run_auto(&r->orch, &r->op, 20000u, &steps));

    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&r->orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, r->sctx.last_terminal);
    TEST_ASSERT_TRUE(snap.cycles_run >= 1u && snap.cycles_run <= 3u);
    float worst = 0.0f;
    for (uint8_t k = 0; k < 32u; ++k) {
        if (fabsf(r->op.wheel.lateral_mm[k]) > worst) worst = fabsf(r->op.wheel.lateral_mm[k]);
    }
    TEST_ASSERT_TRUE(worst <= g_solver.tol_lateral_mm);
    printf("real-model workflow: cycles_run=%u adjustments=%lu final max|lateral|=%.4f mm (tol %.2f)\n",
           (unsigned)snap.cycles_run, (unsigned long)r->op.adjustments_applied, (double)worst, (double)g_solver.tol_lateral_mm);
    /* The provenance carries the real artifact identity. */
    truing_cycle_provenance_t prov;
    truing_orch_provenance(&r->orch, &prov);
    TEST_ASSERT_EQUAL_UINT32(1u, prov.artifact_id);
    TEST_ASSERT_TRUE(truing_fingerprint_equals(&prov.generating_fingerprint, &g_wheel.expected_influence_fingerprint));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, prov.active_layout);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_residual_cost_and_ls_inversion_match_host_per_layout);
    RUN_TEST(test_contract_solve_matches_host_under_tension_absent);
    RUN_TEST(test_contract_refuses_full_when_n_mt_is_unidentified);
    RUN_TEST(test_verify_reports_cost_geometry_and_no_tension_under_tension_absent);
    RUN_TEST(test_exact_linear_state_is_recovered_and_predicted_to_zero);
    RUN_TEST(test_workflow_converges_on_the_real_model_geometry_only);
    return UNITY_END();
}
