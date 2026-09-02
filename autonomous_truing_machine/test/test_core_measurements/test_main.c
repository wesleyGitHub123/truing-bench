/* Measurement chain records and their invariants (SPEC §4.4.1, §6.3, §6.4, §7.4). */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing/measurements.h"

void setUp(void) {}
void tearDown(void) {}

static void test_suspect_provisional_estimate_is_well_formed(void)
{
    truing_tension_estimate_t e;
    test_make_suspect_estimate(&e, 1u, 950.0f, 480.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&e));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_frequency_measurement_check(&e.frequency));
}

static void test_valid_requires_confirmed_mode_identity(void)
{
    /* SPEC §4.4.1: while mode identification is provisional, status MUST NOT be valid. */
    truing_tension_estimate_t e;
    test_make_suspect_estimate(&e, 1u, 950.0f, 480.0f);
    e.meta.status = TRUING_STATUS_VALID;
    e.meta.reason_code = TRUING_REASON_NONE;
    e.frequency.meta.status = TRUING_STATUS_VALID;
    e.frequency.meta.reason_code = TRUING_REASON_NONE;
    /* frequency still presumed_fundamental */
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_VALID_WITHOUT_CONFIRMED_MODE, truing_frequency_measurement_check(&e.frequency));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_EMBEDDED_RECORD, truing_tension_estimate_check(&e));

    truing_tension_estimate_t v;
    test_make_valid_estimate(&v, 1u, 950.0f, 480.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&v));

    /* A valid estimate over a merely-suspect frequency is also rejected. */
    v.frequency.meta.status = TRUING_STATUS_SUSPECT;
    v.frequency.meta.reason_code = TRUING_REASON_LOW_SNR;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_VALID_WITHOUT_CONFIRMED_MODE, truing_tension_estimate_check(&v));
}

static void test_zero_or_negative_tension_never_passes(void)
{
    truing_tension_estimate_t e;
    test_make_suspect_estimate(&e, 1u, 0.0f, 480.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_POSITIVE, truing_tension_estimate_check(&e));
    test_make_suspect_estimate(&e, 1u, -5.0f, 480.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_POSITIVE, truing_tension_estimate_check(&e));
    test_make_suspect_estimate(&e, 1u, NAN, 480.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_FINITE, truing_tension_estimate_check(&e));
}

static void test_model_name_is_mandatory_where_a_value_was_attempted(void)
{
    truing_tension_estimate_t e;
    test_make_suspect_estimate(&e, 1u, 950.0f, 480.0f);
    e.model_name = TRUING_TENSION_MODEL_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_MODEL_UNSET, truing_tension_estimate_check(&e));

    truing_tension_estimate_t r;
    test_make_failed_estimate(&r, 1u, TRUING_STATUS_REJECTED, TRUING_REASON_FREQ_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&r));
    r.model_name = TRUING_TENSION_MODEL_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_MODEL_UNSET, truing_tension_estimate_check(&r));

    /* An unavailable placeholder (capability absent) may legitimately carry no model. */
    truing_tension_estimate_t u;
    test_make_failed_estimate(&u, 1u, TRUING_STATUS_UNAVAILABLE, TRUING_REASON_NOT_IMPLEMENTED);
    u.model_name = TRUING_TENSION_MODEL_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&u));
}

static void test_frequency_measurement_rules(void)
{
    truing_tension_estimate_t e;
    test_make_suspect_estimate(&e, 1u, 950.0f, 480.0f);
    truing_frequency_measurement_t f = e.frequency;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_frequency_measurement_check(&f));
    f.mode_identity = TRUING_MODE_ID_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_MODE_IDENTITY_UNSET, truing_frequency_measurement_check(&f));
    f = e.frequency;
    f.n_candidates = (uint8_t)(TRUING_MAX_CANDIDATE_PEAKS + 1u);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_TOO_MANY_CANDIDATES, truing_frequency_measurement_check(&f));
    f = e.frequency;
    f.selected_frequency_hz = 0.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_POSITIVE, truing_frequency_measurement_check(&f));
    f = e.frequency;
    f.candidates[0].frequency_hz = NAN;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_FINITE, truing_frequency_measurement_check(&f));
    /* A rejected measurement carries no value and need not have one. */
    f = e.frequency;
    f.meta.status = TRUING_STATUS_REJECTED;
    f.meta.reason_code = TRUING_REASON_NO_ONSET_DETECTED;
    f.selected_frequency_hz = NAN;
    f.mode_identity = TRUING_MODE_ID_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_frequency_measurement_check(&f));
}

static void test_runout_measurement_rules(void)
{
    truing_runout_measurement_t m;
    test_make_runout(&m, 2u, 1.0f, 0.12f, -0.03f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_runout_measurement_check(&m));
    /* SPEC §6.3: rim_angle mandatory and in range, regardless of status. */
    m.rim_angle_rad = -0.1f;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_ANGLE_RANGE, truing_runout_measurement_check(&m));
    m.rim_angle_rad = TRUING_TWO_PI;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_ANGLE_RANGE, truing_runout_measurement_check(&m));
    m.rim_angle_rad = NAN;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_FINITE, truing_runout_measurement_check(&m));
    test_make_runout(&m, 2u, 1.0f, NAN, 0.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NOT_FINITE, truing_runout_measurement_check(&m));
    /* Unavailable runout with NaN values is well-formed (P2: status, not a neutral value). */
    test_make_runout(&m, 2u, 1.0f, NAN, NAN);
    m.meta.status = TRUING_STATUS_UNAVAILABLE;
    m.meta.reason_code = TRUING_REASON_NOT_IMPLEMENTED;
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_runout_measurement_check(&m));
}

static void test_string_tables(void)
{
    TEST_ASSERT_EQUAL_STRING("presumed_fundamental", truing_mode_identity_str(TRUING_MODE_ID_PRESUMED_FUNDAMENTAL));
    TEST_ASSERT_EQUAL_STRING("ideal_string", truing_tension_model_str(TRUING_TENSION_MODEL_IDEAL_STRING));
    TEST_ASSERT_EQUAL_STRING("?", truing_tension_model_str((truing_tension_model_t)77));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_suspect_provisional_estimate_is_well_formed);
    RUN_TEST(test_valid_requires_confirmed_mode_identity);
    RUN_TEST(test_zero_or_negative_tension_never_passes);
    RUN_TEST(test_model_name_is_mandatory_where_a_value_was_attempted);
    RUN_TEST(test_frequency_measurement_rules);
    RUN_TEST(test_runout_measurement_rules);
    RUN_TEST(test_string_tables);
    return UNITY_END();
}
