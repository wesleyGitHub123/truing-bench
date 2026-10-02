/* The per-spoke reading store a late-joining page is brought up to date from. */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "truing/survey_readings.h"

static truing_survey_t g;

void setUp(void) { truing_survey_clear(&g); }
void tearDown(void) {}

static void test_an_empty_table_is_an_empty_list(void)
{
    char b[TRUING_SURVEY_JSON_MAX];
    TEST_ASSERT_GREATER_THAN_size_t(0u, truing_survey_json(&g, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("{\"cycle\":0,\"r\":[]}", b);
}

static void test_readings_come_back_in_spoke_order_and_the_last_one_wins(void)
{
    char b[TRUING_SURVEY_JSON_MAX];
    truing_survey_put(&g, 14u, 1u, 359.4f, 477.0f);
    truing_survey_put(&g, 2u, 1u, 400.0f, 600.0f);
    truing_survey_put(&g, 2u, 1u, 375.8f, 522.0f);
    truing_survey_json(&g, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("{\"cycle\":1,\"r\":[[2,375.800,522.000],[14,359.400,477.000]]}", b);
}

static void test_a_missing_value_is_null_not_nan(void)
{
    char b[TRUING_SURVEY_JSON_MAX];
    truing_survey_put(&g, 5u, 2u, 433.3f, NAN);
    truing_survey_put(&g, 6u, 2u, NAN, NAN);
    truing_survey_json(&g, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("{\"cycle\":2,\"r\":[[5,433.300,null],[6,null,null]]}", b);
    TEST_ASSERT_NULL(strstr(b, "nan"));
}

static void test_clear_starts_a_new_session(void)
{
    char b[TRUING_SURVEY_JSON_MAX];
    truing_survey_put(&g, 0u, 1u, 1.0f, 2.0f);
    truing_survey_clear(&g);
    truing_survey_json(&g, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("{\"cycle\":0,\"r\":[]}", b);
}

static void test_all_32_spokes_fit_the_documented_maximum(void)
{
    char b[TRUING_SURVEY_JSON_MAX];
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_survey_put(&g, i, 9u, 1234.567f, 12345.678f);
    }
    const size_t n = truing_survey_json(&g, b, sizeof(b));
    TEST_ASSERT_GREATER_THAN_size_t(0u, n);
    TEST_ASSERT_LESS_THAN_size_t(TRUING_SURVEY_JSON_MAX, n);
    TEST_ASSERT_EQUAL_CHAR('}', b[n - 1u]);
}

static void test_a_buffer_too_small_is_refused_not_truncated(void)
{
    char b[16];
    truing_survey_put(&g, 3u, 1u, 360.0f, 480.0f);
    TEST_ASSERT_EQUAL_size_t(0u, truing_survey_json(&g, b, sizeof(b)));
}

static void test_a_write_in_progress_is_never_copied(void)
{
    char b[TRUING_SURVEY_JSON_MAX];
    truing_survey_put(&g, 3u, 1u, 360.0f, 480.0f);
    g.seq |= 1u;   /* the writer is mid-update and stays there */
    TEST_ASSERT_EQUAL_size_t(0u, truing_survey_json(&g, b, sizeof(b)));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_table_is_an_empty_list);
    RUN_TEST(test_readings_come_back_in_spoke_order_and_the_last_one_wins);
    RUN_TEST(test_a_missing_value_is_null_not_nan);
    RUN_TEST(test_clear_starts_a_new_session);
    RUN_TEST(test_all_32_spokes_fit_the_documented_maximum);
    RUN_TEST(test_a_buffer_too_small_is_refused_not_truncated);
    RUN_TEST(test_a_write_in_progress_is_never_copied);
    return UNITY_END();
}
