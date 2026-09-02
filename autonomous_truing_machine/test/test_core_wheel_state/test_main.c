/* WheelState: pure data with accessors; partial state legal; never zero-filled (SPEC §6.3, §7.4). */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing/wheel_state.h"

static truing_wheel_state_t g_ws;

void setUp(void)
{
    memset(&g_ws, 0, sizeof(g_ws));
}
void tearDown(void) {}

static void test_dimension_validation(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_DIMENSIONS, truing_wheel_state_init(&g_ws, 31u, 31u, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_DIMENSIONS, truing_wheel_state_init(&g_ws, 40u, 40u, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_DIMENSIONS, truing_wheel_state_init(&g_ws, 32u, 0u, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_DIMENSIONS, truing_wheel_state_init(&g_ws, 32u, 37u, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_NULL, truing_wheel_state_init(NULL, 32u, 32u, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 36u, 36u, 0u));
    TEST_ASSERT_TRUE(truing_wheel_dims_supported(32u, 32u));
    TEST_ASSERT_TRUE(truing_wheel_dims_supported(36u, 36u));
    TEST_ASSERT_FALSE(truing_wheel_dims_supported(28u, 28u));
}

static void test_rim_angle_grid(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 0u));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, truing_wheel_state_rim_angle(&g_ws, 0u));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, TRUING_PI / 2.0f, truing_wheel_state_rim_angle(&g_ws, 8u));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, TRUING_PI, truing_wheel_state_rim_angle(&g_ws, 16u));
    TEST_ASSERT_TRUE(isnan(truing_wheel_state_rim_angle(&g_ws, 32u)));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 36u, 36u, 0u));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, TRUING_PI / 2.0f, truing_wheel_state_rim_angle(&g_ws, 9u));
}

static void test_partial_state_is_legal_and_never_zero_filled(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 3u));
    truing_tension_estimate_t e;
    for (uint8_t i = 0; i < 3u; ++i) {
        test_make_suspect_estimate(&e, 3u, 900.0f + (float)i, 470.0f);
        truing_check_t detail = TRUING_CHECK_OK;
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, &detail));
    }
    TEST_ASSERT_EQUAL_UINT8(3u, truing_wheel_state_n_spokes_measured(&g_ws));
    TEST_ASSERT_FALSE(truing_wheel_state_is_complete(&g_ws));
    /* An unmeasured spoke reads back as absent, not as a value. */
    TEST_ASSERT_NULL(truing_wheel_state_spoke(&g_ws, 5u));
    TEST_ASSERT_FALSE(truing_wheel_state_spoke_measured(&g_ws, 5u));
    TEST_ASSERT_NOT_NULL(truing_wheel_state_spoke(&g_ws, 2u));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 902.0f, truing_wheel_state_spoke(&g_ws, 2u)->tension_n);
    /* Out-of-range index. */
    TEST_ASSERT_NULL(truing_wheel_state_spoke(&g_ws, 32u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_INDEX, truing_wheel_state_set_spoke(&g_ws, 32u, &e, NULL));
}

static void test_records_are_validated_on_entry(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 1u));
    truing_tension_estimate_t e;
    truing_check_t detail = TRUING_CHECK_OK;
    test_make_suspect_estimate(&e, 1u, 0.0f, 470.0f);   /* zero tension */
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_RECORD, truing_wheel_state_set_spoke(&g_ws, 0u, &e, &detail));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_POSITIVE, detail);
    TEST_ASSERT_NULL(truing_wheel_state_spoke(&g_ws, 0u));
    /* Wrong cycle. */
    test_make_suspect_estimate(&e, 2u, 900.0f, 470.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_CYCLE_MISMATCH, truing_wheel_state_set_spoke(&g_ws, 0u, &e, &detail));
    /* Failed measurements ARE stored (collection continues, SPEC §13.1). */
    test_make_failed_estimate(&e, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_NO_ONSET_DETECTED);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, 0u, &e, &detail));
    TEST_ASSERT_TRUE(truing_wheel_state_spoke_measured(&g_ws, 0u));
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, truing_wheel_state_spoke(&g_ws, 0u)->meta.status);
}

static void test_runout_rim_angle_must_match_grid(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 1u));
    truing_runout_measurement_t m;
    test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, 4u), 0.1f, 0.02f);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, 4u, &m, NULL));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_RIM_ANGLE_MISMATCH, truing_wheel_state_set_runout(&g_ws, 5u, &m, NULL));
    TEST_ASSERT_NULL(truing_wheel_state_runout(&g_ws, 5u));
    TEST_ASSERT_NOT_NULL(truing_wheel_state_runout(&g_ws, 4u));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_INDEX, truing_wheel_state_set_runout(&g_ws, 32u, &m, NULL));
    m.meta.cycle_index = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WS_ERR_CYCLE_MISMATCH, truing_wheel_state_set_runout(&g_ws, 4u, &m, NULL));
}

static void test_begin_cycle_clears_and_summary_counts(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, 32u, 32u, 1u));
    truing_tension_estimate_t e;
    truing_runout_measurement_t m;
    for (uint8_t i = 0; i < 32u; ++i) {
        if (i % 4u == 3u) {
            test_make_failed_estimate(&e, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_LOW_SNR);
        } else if (i % 4u == 2u) {
            test_make_valid_estimate(&e, 1u, 1000.0f, 500.0f);
        } else {
            test_make_suspect_estimate(&e, 1u, 1000.0f, 500.0f);
        }
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, NULL));
        test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, i), 0.0f, 0.0f);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, i, &m, NULL));
    }
    TEST_ASSERT_TRUE(truing_wheel_state_is_complete(&g_ws));
    truing_wheel_state_summary_t s;
    truing_wheel_state_summarize(&g_ws, &s);
    TEST_ASSERT_EQUAL_UINT8(16u, s.spokes_by_status[TRUING_STATUS_SUSPECT]);
    TEST_ASSERT_EQUAL_UINT8(8u, s.spokes_by_status[TRUING_STATUS_VALID]);
    TEST_ASSERT_EQUAL_UINT8(8u, s.spokes_by_status[TRUING_STATUS_REJECTED]);
    TEST_ASSERT_EQUAL_UINT8(0u, s.spokes_by_status[TRUING_STATUS_UNSET]);
    TEST_ASSERT_EQUAL_UINT8(24u, s.spokes_solver_admissible);
    TEST_ASSERT_EQUAL_UINT8(8u, s.spokes_verification_grade);
    TEST_ASSERT_EQUAL_UINT8(32u, s.runout_by_status[TRUING_STATUS_VALID]);
    TEST_ASSERT_EQUAL_UINT8(32u, s.runout_solver_admissible);

    truing_wheel_state_begin_cycle(&g_ws, 2u);
    TEST_ASSERT_EQUAL_UINT8(2u, g_ws.cycle_index);
    TEST_ASSERT_EQUAL_UINT8(0u, truing_wheel_state_n_spokes_measured(&g_ws));
    TEST_ASSERT_EQUAL_UINT8(0u, truing_wheel_state_n_runout_measured(&g_ws));
    TEST_ASSERT_FALSE(truing_wheel_state_is_complete(&g_ws));
    truing_wheel_state_summarize(&g_ws, &s);
    TEST_ASSERT_EQUAL_UINT8(32u, s.spokes_by_status[TRUING_STATUS_UNSET]);
    /* Cleared runout slots still know their grid angle. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, truing_wheel_state_rim_angle(&g_ws, 7u), g_ws.runout[7].rim_angle_rad);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_dimension_validation);
    RUN_TEST(test_rim_angle_grid);
    RUN_TEST(test_partial_state_is_legal_and_never_zero_filled);
    RUN_TEST(test_records_are_validated_on_entry);
    RUN_TEST(test_runout_rim_angle_must_match_grid);
    RUN_TEST(test_begin_cycle_clears_and_summary_counts);
    return UNITY_END();
}
