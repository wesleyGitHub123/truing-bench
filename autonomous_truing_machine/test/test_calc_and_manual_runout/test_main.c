/* Manual runout entry (SPEC §10.2) and the truing-calculation contract doubles. */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing/admission.h"
#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/runout_if.h"

static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;

void setUp(void)
{
    truing_fake_clock_init(&g_fc, &g_clock, 100u);
}
void tearDown(void) {}

static void test_manual_runout_never_blocks_and_never_fabricates(void)
{
    truing_runout_if_t r;
    truing_runout_manual_ctx_t ctx;
    truing_runout_manual_init(&r, &ctx, g_clock);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_REAL, r.source_impl);   /* a real implementation, not a stub */
    const uint32_t caps = truing_runout_capabilities(&r);
    TEST_ASSERT_TRUE((caps & TRUING_RUNOUT_CAP_MANUAL_ENTRY) != 0u);
    TEST_ASSERT_TRUE((caps & TRUING_RUNOUT_CAP_SNAPSHOT) != 0u);
    TEST_ASSERT_TRUE((caps & TRUING_RUNOUT_CAP_STREAM) == 0u);

    truing_runout_measurement_t m;
    /* No entry yet: reports absence, returns immediately. */
    truing_runout_read_snapshot(&r, 4u, 0.7853982f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, m.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_SENSOR_TIMEOUT, m.meta.reason_code);
    TEST_ASSERT_TRUE(isnan(m.lateral_mm));
    /* Entry for a different index does not satisfy this index. */
    TEST_ASSERT_TRUE(truing_runout_manual_submit(&r, 5u, 0.2f, -0.1f));
    truing_runout_read_snapshot(&r, 4u, 0.7853982f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, m.meta.status);
    /* Matching entry is consumed exactly once. */
    truing_runout_read_snapshot(&r, 5u, 0.9817477f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, m.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_REAL, m.meta.source_impl);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2f, m.lateral_mm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.1f, m.radial_mm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9817477f, m.rim_angle_rad);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_runout_measurement_check(&m));
    truing_runout_read_snapshot(&r, 5u, 0.9817477f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, m.meta.status);
    /* A non-finite entry is rejected, not stored as a number. */
    TEST_ASSERT_TRUE(truing_runout_manual_submit(&r, 6u, NAN, 0.0f));
    truing_runout_read_snapshot(&r, 6u, 1.178097f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, m.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_VALUE_OUT_OF_RANGE, m.meta.reason_code);
    /* Streaming stays unavailable; tare records the operator's zeroing. */
    truing_reason_t reason = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_runout_stream_samples(&r, NULL, NULL, 1u, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, reason);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, truing_runout_tare(&r, &reason));
    TEST_ASSERT_TRUE(ctx.tared);
    /* submit() refuses implementations without manual entry. */
    truing_runout_if_t syn;
    truing_runout_synthetic_ctx_t sctx;
    truing_runout_synthetic_init(&syn, &sctx, g_clock, 32u);
    TEST_ASSERT_FALSE(truing_runout_manual_submit(&syn, 0u, 0.0f, 0.0f));
    TEST_ASSERT_FALSE(truing_runout_manual_submit(NULL, 0u, 0.0f, 0.0f));
}

static void test_calc_stub_reports_no_model(void)
{
    truing_calc_if_t c;
    truing_calc_stub_ctx_t ctx;
    truing_calc_stub_init(&c, &ctx);
    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_fixture_wheel_class_sym32(&wheel);
    truing_fixture_solver_config(&solver, 32u);
    truing_reason_t reason = TRUING_REASON_NONE;
    uint32_t id = 99u;
    TEST_ASSERT_FALSE(c.model_available(&c, &wheel, &solver, &reason, &id, NULL));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, reason);
    TEST_ASSERT_EQUAL_UINT32(0u, id);
    truing_adjustment_plan_t plan;
    truing_admission_t adm;
    memset(&adm, 0, sizeof(adm));
    TEST_ASSERT_FALSE(c.solve(&c, NULL, &adm, &solver, &plan, &reason));
    TEST_ASSERT_FALSE(plan.valid);   /* no plan, never a zero plan */
    TEST_ASSERT_NULL(c.predict_targets);
}

static void test_calc_synthetic_double_follows_admission(void)
{
    truing_calc_if_t c;
    truing_calc_synthetic_ctx_t ctx;
    truing_calc_synthetic_init(&c, &ctx, 42u);
    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_fixture_wheel_class_sym32(&wheel);
    truing_fixture_solver_config(&solver, 32u);
    truing_reason_t reason = TRUING_REASON_NONE;
    uint32_t id = 0u;
    truing_fingerprint_t fp;
    TEST_ASSERT_TRUE(c.model_available(&c, &wheel, &solver, &reason, &id, &fp));
    TEST_ASSERT_EQUAL_UINT32(42u, id);
    TEST_ASSERT_TRUE(fp.set);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, c.select_layout(&c, false, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, c.select_layout(&c, true, &reason));
    ctx.n_mt_identified = false;
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, c.select_layout(&c, false, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, reason);
    ctx.n_mt_identified = true;
    ctx.conditioning_ok_full = false;
    TEST_ASSERT_FALSE(c.conditioning_ok(&c, TRUING_LAYOUT_FULL, &solver, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_ARTIFACT_INVALID, reason);
    TEST_ASSERT_TRUE(c.conditioning_ok(&c, TRUING_LAYOUT_TENSION_ABSENT, &solver, &reason));

    static truing_wheel_state_t ws;
    truing_row_dims_t dims;
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&ws, 32u, 32u, 1u));
    TEST_ASSERT_TRUE(truing_row_dims_init(&dims, 32u, 32u));
    truing_tension_estimate_t e;
    truing_runout_measurement_t m;
    for (uint8_t i = 0; i < 32u; ++i) {
        test_make_suspect_estimate(&e, 1u, 1000.0f, 480.0f);
        truing_wheel_state_set_spoke(&ws, i, &e, NULL);
        test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&ws, i), i == 3u ? 0.4f : 0.0f, 0.0f);
        truing_wheel_state_set_runout(&ws, i, &m, NULL);
    }
    truing_admission_t adm;
    truing_admission_evaluate(&ws, &dims, TRUING_LAYOUT_FULL, TRUING_REASON_NONE, &adm);
    TEST_ASSERT_TRUE(adm.admissible);
    truing_adjustment_plan_t plan;
    ctx.turns_per_mm_lateral = 2.0f;
    TEST_ASSERT_TRUE(c.solve(&c, &ws, &adm, &solver, &plan, &reason));
    TEST_ASSERT_TRUE(plan.valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.8f, plan.turns_rev[3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, plan.turns_rev[4]);
    TEST_ASSERT_EQUAL_UINT32(42u, plan.artifact_id);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, plan.active_layout);
    TEST_ASSERT_EQUAL_UINT16(96u, plan.n_active_rows);
    TEST_ASSERT_EQUAL_UINT8(32u, plan.n_suspect_rows);
    TEST_ASSERT_FALSE(plan.mean_tension_targeting_applied);
    /* Cost: one lateral residual of 0.4/0.1 = 4 -> 16 over 96 rows. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 16.0f / 96.0f, plan.cost_j);
    truing_verification_result_t v;
    TEST_ASSERT_TRUE(c.verify(&c, &ws, &adm, &solver, &v, &reason));
    TEST_ASSERT_FALSE(v.geometric_converged);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, v.max_lateral_mm);
    TEST_ASSERT_TRUE(v.tension_evaluated);
    TEST_ASSERT_TRUE(v.tension_compliant);          /* uniform 1000 N at target 1000 N */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, v.non_uniformity_side_a);
    /* A refused admission yields no plan. */
    adm.admissible = false;
    TEST_ASSERT_FALSE(c.solve(&c, &ws, &adm, &solver, &plan, &reason));
    TEST_ASSERT_FALSE(plan.valid);
    TEST_ASSERT_FALSE(c.verify(&c, &ws, &adm, &solver, &v, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PARTIAL_WHEEL_STATE, v.reason);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_manual_runout_never_blocks_and_never_fabricates);
    RUN_TEST(test_calc_stub_reports_no_model);
    RUN_TEST(test_calc_synthetic_double_follows_admission);
    return UNITY_END();
}
