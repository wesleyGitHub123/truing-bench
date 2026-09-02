/* Solver admission rules R1/R2/R4 with policy exclusion (SPEC §8.11), plan safety and
 * deadband (SPEC §7.5, §11.2), and session-parameter application (SPEC §12.3.1). */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing/admission.h"
#include "truing/params.h"
#include "truing/plan.h"
#include "truing_fixtures/fixtures.h"

static truing_wheel_state_t g_ws;
static truing_row_dims_t g_dims;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 1u));
    TEST_ASSERT_TRUE(truing_row_dims_init(&g_dims, 32u, 32u));
}
void tearDown(void) {}

static void fill_complete(void)
{
    truing_tension_estimate_t e;
    truing_runout_measurement_t m;
    for (uint8_t i = 0; i < 32u; ++i) {
        test_make_suspect_estimate(&e, 1u, 1000.0f, 480.0f);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, NULL));
        test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, i), 0.05f * (float)(i % 3), 0.01f);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, i, &m, NULL));
    }
}

static void test_complete_state_admits_full(void)
{
    fill_complete();
    truing_admission_t a;
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &a);
    TEST_ASSERT_TRUE(a.admissible);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, a.layout);
    TEST_ASSERT_EQUAL_UINT16(96u, a.n_active_rows);
    TEST_ASSERT_EQUAL_UINT8(64u, a.n_valid_rows);      /* 32 lateral + 32 radial rows, valid */
    TEST_ASSERT_EQUAL_UINT8(32u, a.n_suspect_rows);    /* provisional tension rows */
    TEST_ASSERT_EQUAL_UINT16(0u, truing_row_mask_popcount(&a.missing_rows));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NONE, a.reason);
    TEST_ASSERT_FALSE(a.tension_excluded_by_policy);
}

static void test_policy_exclusion_selects_tension_absent_without_touching_measurements(void)
{
    fill_complete();
    truing_admission_t a;
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_TENSION_ABSENT, TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, &a);
    TEST_ASSERT_TRUE(a.admissible);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, a.layout);
    TEST_ASSERT_EQUAL_UINT16(64u, a.n_active_rows);
    TEST_ASSERT_EQUAL_UINT8(0u, a.n_suspect_rows);     /* tension rows are not in the active set */
    TEST_ASSERT_TRUE(a.tension_excluded_by_policy);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, a.policy_reason);
    TEST_ASSERT_EQUAL_UINT16(96u, truing_row_mask_popcount(&a.available_rows));   /* data untouched */
    truing_wheel_state_summary_t s;
    truing_wheel_state_summarize(&g_ws, &s);
    TEST_ASSERT_EQUAL_UINT8(32u, s.spokes_by_status[TRUING_STATUS_SUSPECT]);
}

static void test_all_tension_absent_selects_tension_absent_by_availability(void)
{
    fill_complete();
    truing_tension_estimate_t e;
    for (uint8_t i = 0; i < 32u; ++i) {
        test_make_failed_estimate(&e, 1u, TRUING_STATUS_UNAVAILABLE, TRUING_REASON_NOT_IMPLEMENTED);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, NULL));
    }
    truing_admission_t a;
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &a);
    TEST_ASSERT_TRUE(a.admissible);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, a.layout);
    TEST_ASSERT_FALSE(a.tension_excluded_by_policy);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NONE, a.policy_reason);
}

static void test_partial_tension_refused_with_missing_rows_identified(void)
{
    fill_complete();
    truing_tension_estimate_t e;
    test_make_failed_estimate(&e, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_LOW_SNR);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, 7u, &e, NULL));
    test_make_failed_estimate(&e, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_NO_ONSET_DETECTED);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, 20u, &e, NULL));
    truing_admission_t a;
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &a);
    TEST_ASSERT_FALSE(a.admissible);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PARTIAL_WHEEL_STATE, a.reason);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, a.layout);
    TEST_ASSERT_EQUAL_UINT16(2u, truing_row_mask_popcount(&a.missing_rows));
    TEST_ASSERT_TRUE(truing_admission_missing_spoke(&a, &g_dims, 7u));
    TEST_ASSERT_TRUE(truing_admission_missing_spoke(&a, &g_dims, 20u));
    TEST_ASSERT_FALSE(truing_admission_missing_spoke(&a, &g_dims, 8u));
    TEST_ASSERT_FALSE(truing_admission_missing_rim_index(&a, &g_dims, 7u));
    /* Policy exclusion of tension rescues the state: the missing rows are no longer needed. */
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_TENSION_ABSENT, TRUING_REASON_NONE, &a);
    TEST_ASSERT_TRUE(a.admissible);
}

static void test_missing_runout_refused_and_identified(void)
{
    fill_complete();
    truing_runout_measurement_t m;
    test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, 3u), 0.0f, 0.0f);
    m.meta.status = TRUING_STATUS_UNAVAILABLE;
    m.meta.reason_code = TRUING_REASON_SENSOR_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, 3u, &m, NULL));
    truing_admission_t a;
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &a);
    TEST_ASSERT_FALSE(a.admissible);
    TEST_ASSERT_EQUAL_UINT16(2u, truing_row_mask_popcount(&a.missing_rows));   /* lateral + radial of index 3 */
    TEST_ASSERT_TRUE(truing_admission_missing_rim_index(&a, &g_dims, 3u));
    TEST_ASSERT_FALSE(truing_admission_missing_spoke(&a, &g_dims, 3u));
    /* Unmeasured state: every row missing, R2 fails as well. */
    truing_wheel_state_begin_cycle(&g_ws, 2u);
    truing_admission_evaluate(&g_ws, &g_dims, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &a);
    TEST_ASSERT_FALSE(a.admissible);
    TEST_ASSERT_EQUAL_UINT16(0u, a.n_active_rows);
    /* Dimension mismatch is never admissible. */
    truing_row_dims_t d36;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d36, 36u, 36u));
    truing_admission_evaluate(&g_ws, &d36, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &a);
    TEST_ASSERT_FALSE(a.admissible);
}

static void test_plan_safety_and_deadband(void)
{
    fill_complete();
    truing_solver_config_t cfg;
    truing_fixture_solver_config(&cfg, 32u);   /* max 1.0 rev, deadband 0.05 rev, tol 0.1 / 0.05 mm */
    truing_adjustment_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.valid = true;
    plan.n_spokes = 32u;
    for (uint8_t i = 0; i < 32u; ++i) {
        plan.turns_rev[i] = 0.02f;   /* all below the deadband */
    }
    plan.turns_rev[5] = 0.5f;
    uint8_t bad = 0xFFu;
    TEST_ASSERT_FALSE(truing_plan_is_unsafe(&plan, &cfg, &bad));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, truing_plan_max_abs_turn(&plan));
    /* Rim index 2 is pushed out of tolerance (0.3 mm > tol 0.1); every other index sits at
     * 0.00 / 0.05 / 0.10 mm, all within tolerance (0.10 == tol counts as within). */
    truing_runout_measurement_t out_of_tol;
    test_make_runout(&out_of_tol, 1u, truing_wheel_state_rim_angle(&g_ws, 2u), 0.3f, 0.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, 2u, &out_of_tol, NULL));
    const uint8_t skipped = truing_plan_apply_deadband(&plan, &cfg, &g_ws);
    TEST_ASSERT_EQUAL_UINT8(30u, skipped);           /* all small turns except index 2; index 5 is large */
    TEST_ASSERT_FALSE(plan.skipped[5]);              /* large turn never skipped */
    TEST_ASSERT_TRUE(plan.skipped[0]);               /* small turn, lateral 0.00 -> within tolerance */
    TEST_ASSERT_TRUE(plan.skipped[1]);               /* small turn, lateral 0.05 -> within tolerance */
    TEST_ASSERT_FALSE(plan.skipped[2]);              /* small turn but the wheel is out of tolerance there */
    TEST_ASSERT_EQUAL_UINT8(2u, truing_plan_count_pending(&plan));
    plan.turns_rev[9] = -1.5f;
    TEST_ASSERT_TRUE(truing_plan_is_unsafe(&plan, &cfg, &bad));
    TEST_ASSERT_EQUAL_UINT8(9u, bad);
    plan.turns_rev[9] = NAN;
    TEST_ASSERT_TRUE(truing_plan_is_unsafe(&plan, &cfg, &bad));
    plan.valid = false;
    TEST_ASSERT_FALSE(truing_plan_is_unsafe(&plan, &cfg, &bad));
    TEST_ASSERT_EQUAL_UINT8(0u, truing_plan_count_pending(&plan));
}

static void test_param_apply_respects_classes(void)
{
    truing_solver_config_t cfg;
    truing_fixture_solver_config(&cfg, 32u);
    TEST_ASSERT_TRUE(truing_param_apply(&cfg, TRUING_PARAM_MAX_CYCLES, 9.0f));
    TEST_ASSERT_EQUAL_UINT8(9u, cfg.max_cycles);
    TEST_ASSERT_TRUE(truing_param_apply(&cfg, TRUING_PARAM_TARGET_TENSION, 1100.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1100.0f, cfg.target_tension_n);
    TEST_ASSERT_TRUE(truing_param_apply(&cfg, TRUING_PARAM_EXCITATION_SETTLE_MS, 750.0f));
    TEST_ASSERT_EQUAL_UINT16(750u, cfg.excitation_settle_ms);
    /* Artifact-bound values are never applied (SPEC §12.3.1). */
    const float tol_before = cfg.tol_lateral_mm;
    TEST_ASSERT_FALSE(truing_param_apply(&cfg, TRUING_PARAM_TOL_LATERAL, 0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, tol_before, cfg.tol_lateral_mm);
    TEST_ASSERT_FALSE(truing_param_apply(&cfg, TRUING_PARAM_UNSET, 1.0f));
    /* Values that do not fit the field are refused. */
    TEST_ASSERT_FALSE(truing_param_apply(&cfg, TRUING_PARAM_MAX_CYCLES, 300.0f));
    TEST_ASSERT_FALSE(truing_param_apply(&cfg, TRUING_PARAM_MAX_CYCLES, 2.5f));
    TEST_ASSERT_FALSE(truing_param_apply(&cfg, TRUING_PARAM_TARGET_TENSION, NAN));
    TEST_ASSERT_EQUAL_UINT8(9u, cfg.max_cycles);
    /* Applied values still go through validation by the caller: a zero max_cycles is representable... */
    TEST_ASSERT_TRUE(truing_param_apply(&cfg, TRUING_PARAM_MAX_CYCLES, 0.0f));
    /* ...and rejected there. */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_solver_config_check(&cfg, NULL));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_complete_state_admits_full);
    RUN_TEST(test_policy_exclusion_selects_tension_absent_without_touching_measurements);
    RUN_TEST(test_all_tension_absent_selects_tension_absent_by_availability);
    RUN_TEST(test_partial_tension_refused_with_missing_rows_identified);
    RUN_TEST(test_missing_runout_refused_and_identified);
    RUN_TEST(test_plan_safety_and_deadband);
    RUN_TEST(test_param_apply_respects_classes);
    return UNITY_END();
}
