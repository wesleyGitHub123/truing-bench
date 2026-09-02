/* Status / reason / terminal / provenance enumerations and per-record meta checks (SPEC §6.2, §13.2). */
#include <string.h>
#include <unity.h>

#include "truing/status.h"

void setUp(void) {}
void tearDown(void) {}

static truing_record_meta_t meta(truing_status_t s, truing_reason_t r, truing_source_impl_t src)
{
    truing_record_meta_t m;
    memset(&m, 0, sizeof(m));
    m.status = s;
    m.reason_code = r;
    m.source_impl = src;
    m.timestamp_ms = 1234u;
    return m;
}

static void test_zero_initialised_record_is_invalid(void)
{
    truing_record_meta_t m;
    memset(&m, 0, sizeof(m));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_STATUS_UNSET, truing_record_meta_check(&m));
}

static void test_reason_required_when_not_valid(void)
{
    truing_record_meta_t m = meta(TRUING_STATUS_SUSPECT, TRUING_REASON_NONE, TRUING_SOURCE_REAL);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_REASON_REQUIRED, truing_record_meta_check(&m));
    m = meta(TRUING_STATUS_UNAVAILABLE, TRUING_REASON_NONE, TRUING_SOURCE_REAL);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_REASON_REQUIRED, truing_record_meta_check(&m));
}

static void test_reason_forbidden_on_valid(void)
{
    truing_record_meta_t m = meta(TRUING_STATUS_VALID, TRUING_REASON_LOW_SNR, TRUING_SOURCE_REAL);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_REASON_ON_VALID, truing_record_meta_check(&m));
}

static void test_source_required(void)
{
    truing_record_meta_t m = meta(TRUING_STATUS_VALID, TRUING_REASON_NONE, TRUING_SOURCE_UNSET);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_SOURCE_UNSET, truing_record_meta_check(&m));
}

static void test_well_formed_records(void)
{
    truing_record_meta_t m = meta(TRUING_STATUS_VALID, TRUING_REASON_NONE, TRUING_SOURCE_REAL);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_record_meta_check(&m));
    m = meta(TRUING_STATUS_SUSPECT, TRUING_REASON_PROVISIONAL_MODE_ID, TRUING_SOURCE_SYNTHETIC);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_record_meta_check(&m));
    m = meta(TRUING_STATUS_REJECTED, TRUING_REASON_NO_ONSET_DETECTED, TRUING_SOURCE_RECORDED);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_record_meta_check(&m));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_NULL, truing_record_meta_check(NULL));
}

static void test_out_of_range_enums_rejected(void)
{
    truing_record_meta_t m = meta((truing_status_t)99, TRUING_REASON_NONE, TRUING_SOURCE_REAL);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_STATUS_RANGE, truing_record_meta_check(&m));
    m = meta(TRUING_STATUS_SUSPECT, (truing_reason_t)99, TRUING_SOURCE_REAL);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_ERR_REASON_RANGE, truing_record_meta_check(&m));
}

static void test_admissibility_bars(void)
{
    /* SPEC §8.11 solver-admissible: valid or suspect. */
    TEST_ASSERT_TRUE(truing_status_is_solver_admissible(TRUING_STATUS_VALID));
    TEST_ASSERT_TRUE(truing_status_is_solver_admissible(TRUING_STATUS_SUSPECT));
    TEST_ASSERT_FALSE(truing_status_is_solver_admissible(TRUING_STATUS_REJECTED));
    TEST_ASSERT_FALSE(truing_status_is_solver_admissible(TRUING_STATUS_UNAVAILABLE));
    TEST_ASSERT_FALSE(truing_status_is_solver_admissible(TRUING_STATUS_STALE));
    TEST_ASSERT_FALSE(truing_status_is_solver_admissible(TRUING_STATUS_UNSET));
    /* SPEC §8.6.3 verification-grade: valid only. */
    TEST_ASSERT_TRUE(truing_status_is_verification_grade(TRUING_STATUS_VALID));
    TEST_ASSERT_FALSE(truing_status_is_verification_grade(TRUING_STATUS_SUSPECT));
}

static void test_terminal_vocabulary(void)
{
    /* SPEC §8.6.2: CONVERGED_GEOMETRIC_ONLY must never read as success. */
    TEST_ASSERT_TRUE(truing_terminal_is_success(TRUING_TERMINAL_CONVERGED));
    TEST_ASSERT_FALSE(truing_terminal_is_success(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY));
    TEST_ASSERT_FALSE(truing_terminal_is_abort(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY));
    TEST_ASSERT_TRUE(truing_terminal_is_abort(TRUING_TERMINAL_ABORT_NO_PROGRESS));
    TEST_ASSERT_TRUE(truing_terminal_is_abort(TRUING_TERMINAL_ABORT_PARTIAL_STATE));
    TEST_ASSERT_EQUAL_STRING("CONVERGED_GEOMETRIC_ONLY", truing_terminal_str(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY));
    TEST_ASSERT_EQUAL_STRING("ABORT_UNSAFE_ADJUSTMENT", truing_terminal_str(TRUING_TERMINAL_ABORT_UNSAFE_ADJUSTMENT));
}

static void test_string_tables_cover_every_value(void)
{
    for (int i = 0; i < (int)TRUING_STATUS__COUNT; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", truing_status_str((truing_status_t)i)));
    }
    for (int i = 0; i < (int)TRUING_REASON__COUNT; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", truing_reason_str((truing_reason_t)i)));
    }
    for (int i = 0; i < (int)TRUING_TERMINAL__COUNT; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", truing_terminal_str((truing_terminal_result_t)i)));
    }
    for (int i = 0; i < (int)TRUING_SOURCE__COUNT; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", truing_source_impl_str((truing_source_impl_t)i)));
    }
    for (int i = 0; i < (int)TRUING_CHECK__COUNT; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", truing_check_str((truing_check_t)i)));
    }
    TEST_ASSERT_EQUAL_STRING("?", truing_reason_str((truing_reason_t)TRUING_REASON__COUNT));
    /* SPEC §13.2 names, verbatim. */
    TEST_ASSERT_EQUAL_STRING("PROVISIONAL_MODE_ID", truing_reason_str(TRUING_REASON_PROVISIONAL_MODE_ID));
    TEST_ASSERT_EQUAL_STRING("REQUIRES_ARTIFACT_REGENERATION", truing_reason_str(TRUING_REASON_REQUIRES_ARTIFACT_REGENERATION));
    TEST_ASSERT_EQUAL_STRING("MEAN_TENSION_MODEL_UNAVAILABLE", truing_reason_str(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE));
    TEST_ASSERT_EQUAL_STRING("TENSION_NOT_VERIFICATION_GRADE", truing_reason_str(TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE));
    TEST_ASSERT_EQUAL_STRING("STALE_INTENT", truing_reason_str(TRUING_REASON_STALE_INTENT));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_zero_initialised_record_is_invalid);
    RUN_TEST(test_reason_required_when_not_valid);
    RUN_TEST(test_reason_forbidden_on_valid);
    RUN_TEST(test_source_required);
    RUN_TEST(test_well_formed_records);
    RUN_TEST(test_out_of_range_enums_rejected);
    RUN_TEST(test_admissibility_bars);
    RUN_TEST(test_terminal_vocabulary);
    RUN_TEST(test_string_tables_cover_every_value);
    return UNITY_END();
}
