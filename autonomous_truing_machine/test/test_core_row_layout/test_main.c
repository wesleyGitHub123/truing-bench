/* Row indexing, masks, shipped layouts, availability vs. active row set (SPEC §8.7, §8.7.1, §8.11). */
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing/row_layout.h"

static truing_wheel_state_t g_ws;

void setUp(void)
{
    memset(&g_ws, 0, sizeof(g_ws));
}
void tearDown(void) {}

static void fill_state(uint8_t n)
{
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&g_ws, n, n, 1u));
    truing_tension_estimate_t e;
    truing_runout_measurement_t m;
    for (uint8_t i = 0; i < n; ++i) {
        test_make_suspect_estimate(&e, 1u, 950.0f, 480.0f);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, NULL));
        test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, i), 0.01f, -0.02f);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, i, &m, NULL));
    }
}

static void test_dimensions_derive_from_config(void)
{
    truing_row_dims_t d;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_EQUAL_UINT16(96u, d.n_full_rows);
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 36u, 36u));
    TEST_ASSERT_EQUAL_UINT16(108u, d.n_full_rows);
    TEST_ASSERT_EQUAL_UINT16(108u, TRUING_MAX_FULL_ROWS);
    TEST_ASSERT_FALSE(truing_row_dims_init(&d, 30u, 30u));
    TEST_ASSERT_EQUAL_UINT16(0u, d.n_full_rows);
}

static void test_row_indexing_and_channel_decode(void)
{
    truing_row_dims_t d;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_EQUAL_UINT16(5u, truing_row_lateral(&d, 5u));
    TEST_ASSERT_EQUAL_UINT16(37u, truing_row_radial(&d, 5u));
    TEST_ASSERT_EQUAL_UINT16(69u, truing_row_tension(&d, 5u));
    uint8_t idx = 0xFFu;
    TEST_ASSERT_EQUAL_INT(TRUING_ROW_CHANNEL_LATERAL, truing_row_channel(&d, 31u, &idx));
    TEST_ASSERT_EQUAL_UINT8(31u, idx);
    TEST_ASSERT_EQUAL_INT(TRUING_ROW_CHANNEL_RADIAL, truing_row_channel(&d, 32u, &idx));
    TEST_ASSERT_EQUAL_UINT8(0u, idx);
    TEST_ASSERT_EQUAL_INT(TRUING_ROW_CHANNEL_TENSION, truing_row_channel(&d, 95u, &idx));
    TEST_ASSERT_EQUAL_UINT8(31u, idx);
    TEST_ASSERT_EQUAL_INT(TRUING_ROW_CHANNEL_INVALID, truing_row_channel(&d, 96u, &idx));
}

static void test_masks_and_shipped_layouts(void)
{
    truing_row_dims_t d;
    truing_row_mask_t full, ta, none;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_TRUE(truing_row_mask_for_layout(TRUING_LAYOUT_FULL, &d, &full));
    TEST_ASSERT_TRUE(truing_row_mask_for_layout(TRUING_LAYOUT_TENSION_ABSENT, &d, &ta));
    TEST_ASSERT_FALSE(truing_row_mask_for_layout(TRUING_LAYOUT_NONE, &d, &none));
    TEST_ASSERT_EQUAL_UINT16(96u, truing_row_mask_popcount(&full));
    TEST_ASSERT_EQUAL_UINT16(64u, truing_row_mask_popcount(&ta));
    TEST_ASSERT_EQUAL_UINT16(0u, truing_row_mask_popcount(&none));
    TEST_ASSERT_EQUAL_UINT16(96u, truing_layout_row_count(TRUING_LAYOUT_FULL, &d));
    TEST_ASSERT_EQUAL_UINT16(64u, truing_layout_row_count(TRUING_LAYOUT_TENSION_ABSENT, &d));
    TEST_ASSERT_FALSE(truing_row_mask_equals(&full, &ta));
    TEST_ASSERT_TRUE(truing_row_mask_test(&full, 95u));
    TEST_ASSERT_FALSE(truing_row_mask_test(&ta, 64u));
    TEST_ASSERT_FALSE(truing_row_mask_test(&ta, 200u));   /* out of range: never set, never read */
    /* 36-spoke wheel: 36 angles, 108 / 72 rows (SPEC §8.9: not 32). */
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 36u, 36u));
    TEST_ASSERT_EQUAL_UINT16(108u, truing_layout_row_count(TRUING_LAYOUT_FULL, &d));
    TEST_ASSERT_EQUAL_UINT16(72u, truing_layout_row_count(TRUING_LAYOUT_TENSION_ABSENT, &d));
    TEST_ASSERT_EQUAL_STRING("TENSION_ABSENT", truing_layout_str(TRUING_LAYOUT_TENSION_ABSENT));
}

static void test_complete_state_matches_full(void)
{
    fill_state(32u);
    truing_row_dims_t d;
    truing_row_mask_t avail;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_TRUE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_UINT16(96u, truing_row_mask_popcount(&avail));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, truing_layout_match(&avail, &d));
    truing_row_channel_summary_t s;
    truing_row_mask_summarize(&avail, &d, &s);
    TEST_ASSERT_TRUE(s.lateral_complete && s.radial_complete && s.tension_complete);
    TEST_ASSERT_FALSE(s.tension_empty);
}

static void test_all_tension_rejected_matches_tension_absent(void)
{
    fill_state(32u);
    truing_tension_estimate_t e;
    for (uint8_t i = 0; i < 32u; ++i) {
        test_make_failed_estimate(&e, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_NO_ONSET_DETECTED);
        TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, i, &e, NULL));
    }
    truing_row_dims_t d;
    truing_row_mask_t avail;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_TRUE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_UINT16(64u, truing_row_mask_popcount(&avail));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, truing_layout_match(&avail, &d));
}

static void test_partial_tension_matches_no_layout(void)
{
    /* SPEC §8.11 R4: some tension rows missing -> matches neither -> PARTIAL_WHEEL_STATE. */
    fill_state(32u);
    truing_tension_estimate_t e;
    test_make_failed_estimate(&e, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_LOW_SNR);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&g_ws, 7u, &e, NULL));
    truing_row_dims_t d;
    truing_row_mask_t avail;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_TRUE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_UINT16(95u, truing_row_mask_popcount(&avail));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_NONE, truing_layout_match(&avail, &d));
    /* An unmeasured spoke behaves the same. */
    fill_state(32u);
    truing_wheel_state_begin_cycle(&g_ws, 2u);
    TEST_ASSERT_TRUE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_UINT16(0u, truing_row_mask_popcount(&avail));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_NONE, truing_layout_match(&avail, &d));
}

static void test_one_rejected_runout_matches_no_layout(void)
{
    fill_state(32u);
    truing_runout_measurement_t m;
    test_make_runout(&m, 1u, truing_wheel_state_rim_angle(&g_ws, 3u), 0.0f, 0.0f);
    m.meta.status = TRUING_STATUS_REJECTED;
    m.meta.reason_code = TRUING_REASON_VALUE_OUT_OF_RANGE;
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&g_ws, 3u, &m, NULL));
    truing_row_dims_t d;
    truing_row_mask_t avail;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_TRUE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_UINT16(94u, truing_row_mask_popcount(&avail));   /* lateral + radial rows gone */
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_NONE, truing_layout_match(&avail, &d));
    truing_row_channel_summary_t s;
    truing_row_mask_summarize(&avail, &d, &s);
    TEST_ASSERT_EQUAL_UINT16(31u, s.n_lateral);
    TEST_ASSERT_EQUAL_UINT16(31u, s.n_radial);
    TEST_ASSERT_FALSE(s.lateral_complete);
}

static void test_policy_exclusion_is_representable(void)
{
    /* SPEC §8.11: the active row set may exclude the whole tension channel even when every
     * tension measurement succeeded (n_mt unidentified / geometry-only policy). */
    fill_state(32u);
    truing_row_dims_t d;
    truing_row_mask_t avail, active;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_TRUE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, truing_layout_match(&avail, &d));
    TEST_ASSERT_TRUE(truing_row_mask_for_layout(TRUING_LAYOUT_TENSION_ABSENT, &d, &active));
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, truing_layout_match(&active, &d));
    /* Measurements keep their true status: availability is unchanged by policy. */
    truing_wheel_state_summary_t ws;
    truing_wheel_state_summarize(&g_ws, &ws);
    TEST_ASSERT_EQUAL_UINT8(32u, ws.spokes_solver_admissible);
}

static void test_dimension_mismatch_is_refused(void)
{
    fill_state(36u);
    truing_row_dims_t d;
    truing_row_mask_t avail;
    TEST_ASSERT_TRUE(truing_row_dims_init(&d, 32u, 32u));
    TEST_ASSERT_FALSE(truing_row_mask_available(&g_ws, &d, &avail));
    TEST_ASSERT_EQUAL_UINT16(0u, truing_row_mask_popcount(&avail));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_dimensions_derive_from_config);
    RUN_TEST(test_row_indexing_and_channel_decode);
    RUN_TEST(test_masks_and_shipped_layouts);
    RUN_TEST(test_complete_state_matches_full);
    RUN_TEST(test_all_tension_rejected_matches_tension_absent);
    RUN_TEST(test_partial_tension_matches_no_layout);
    RUN_TEST(test_one_rejected_runout_matches_no_layout);
    RUN_TEST(test_policy_exclusion_is_representable);
    RUN_TEST(test_dimension_mismatch_is_refused);
    return UNITY_END();
}
