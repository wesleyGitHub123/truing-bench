/* Parameter mutability classes and SET_PARAMETER admissibility = f(state, class) (SPEC §12.3.1). */
#include <unity.h>

#include "truing/params.h"

void setUp(void) {}
void tearDown(void) {}

static void test_every_runtime_parameter_is_classified(void)
{
    /* SPEC §12.3.1: no runtime-selectable field is unclassified. */
    for (int id = 1; id < (int)TRUING_PARAM__COUNT; ++id) {
        TEST_ASSERT_TRUE_MESSAGE(truing_param_is_classified((truing_param_id_t)id), truing_param_str((truing_param_id_t)id));
    }
    TEST_ASSERT_FALSE(truing_param_is_classified(TRUING_PARAM_UNSET));
    TEST_ASSERT_FALSE(truing_param_is_classified((truing_param_id_t)TRUING_PARAM__COUNT));
}

static void test_class_assignments(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_SESSION_MUTABLE, truing_param_class(TRUING_PARAM_MAX_CYCLES));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_SESSION_MUTABLE, truing_param_class(TRUING_PARAM_PARTIAL_STATE_REMEASURE_ATTEMPTS));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_SESSION_FIXED, truing_param_class(TRUING_PARAM_TARGET_TENSION));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_SESSION_FIXED, truing_param_class(TRUING_PARAM_TENSION_MODEL_PROFILE_ID));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_SESSION_FIXED, truing_param_class(TRUING_PARAM_ADJUSTMENT_DEADBAND));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_SESSION_FIXED, truing_param_class(TRUING_PARAM_MAX_ADJUSTMENT_REVOLUTIONS));
    /* Tolerances and trust weights look like knobs and are not (SPEC §12.3.1). */
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_ARTIFACT_BOUND, truing_param_class(TRUING_PARAM_TOL_LATERAL));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_ARTIFACT_BOUND, truing_param_class(TRUING_PARAM_TRUST_TENSION));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_ARTIFACT_BOUND, truing_param_class(TRUING_PARAM_N_RIM_ANGLES));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_ARTIFACT_BOUND, truing_param_class(TRUING_PARAM_EXPECTED_INFLUENCE_FINGERPRINT));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_CLASS_ARTIFACT_BOUND, truing_param_class(TRUING_PARAM_C_SIDE_A));
    TEST_ASSERT_EQUAL_STRING("target_tension", truing_param_str(TRUING_PARAM_TARGET_TENSION));
}

static void test_admissibility_matrix(void)
{
    /* READY: session-mutable AND session-fixed accepted. */
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_ACCEPT, truing_set_parameter_admissible(TRUING_STATE_READY, TRUING_PARAM_MAX_CYCLES));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_ACCEPT, truing_set_parameter_admissible(TRUING_STATE_READY, TRUING_PARAM_TARGET_TENSION));
    /* WAIT_FOR_OPERATOR: session-mutable only. */
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_ACCEPT, truing_set_parameter_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, TRUING_PARAM_MAX_CYCLES));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_SESSION_IN_PROGRESS,
                          truing_set_parameter_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, TRUING_PARAM_TARGET_TENSION));
    /* Autonomous states: nothing. */
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_AUTONOMOUS_STATE,
                          truing_set_parameter_admissible(TRUING_STATE_COMPUTE_ADJUSTMENTS, TRUING_PARAM_MAX_CYCLES));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_AUTONOMOUS_STATE,
                          truing_set_parameter_admissible(TRUING_STATE_MEASURE_SPOKE_TENSION, TRUING_PARAM_TARGET_TENSION));
    /* Artifact-bound: always refused, in every state, with the defined reason code. */
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_REQUIRES_ARTIFACT_REGENERATION,
                          truing_set_parameter_admissible(TRUING_STATE_READY, TRUING_PARAM_TOL_LATERAL));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_REQUIRES_ARTIFACT_REGENERATION,
                          truing_set_parameter_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, TRUING_PARAM_ASYMMETRIC));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_REQUIRES_ARTIFACT_REGENERATION,
                          truing_set_param_verdict_reason(TRUING_SET_PARAM_REJECT_REQUIRES_ARTIFACT_REGENERATION));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NONE, truing_set_param_verdict_reason(TRUING_SET_PARAM_REJECT_SESSION_IN_PROGRESS));
    /* Inactive states and unknown parameters. */
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_STATE_INACTIVE, truing_set_parameter_admissible(TRUING_STATE_BOOT, TRUING_PARAM_MAX_CYCLES));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_STATE_INACTIVE, truing_set_parameter_admissible(TRUING_STATE_TERMINAL, TRUING_PARAM_MAX_CYCLES));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_UNKNOWN_PARAM, truing_set_parameter_admissible(TRUING_STATE_READY, TRUING_PARAM_UNSET));
    TEST_ASSERT_EQUAL_INT(TRUING_SET_PARAM_REJECT_UNKNOWN_PARAM, truing_set_parameter_admissible(TRUING_STATE_READY, (truing_param_id_t)200));
}

static void test_state_classification(void)
{
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_CLASS_INACTIVE, truing_state_class(TRUING_STATE_BOOT));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_CLASS_INACTIVE, truing_state_class(TRUING_STATE_INITIALIZE));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_CLASS_INACTIVE, truing_state_class(TRUING_STATE_TERMINAL));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_CLASS_READY, truing_state_class(TRUING_STATE_READY));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_CLASS_WAIT_FOR_OPERATOR, truing_state_class(TRUING_STATE_WAIT_FOR_OPERATOR));
    for (int s = (int)TRUING_STATE_MEASURE_WHEEL_STATE; s < (int)TRUING_STATE_TERMINAL; ++s) {
        if (s == (int)TRUING_STATE_WAIT_FOR_OPERATOR) {
            continue;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(TRUING_STATE_CLASS_AUTONOMOUS, truing_state_class((truing_state_t)s), truing_state_str((truing_state_t)s));
        TEST_ASSERT_TRUE(truing_state_is_operational((truing_state_t)s));
    }
    TEST_ASSERT_TRUE(truing_state_is_operational(TRUING_STATE_WAIT_FOR_OPERATOR));
    TEST_ASSERT_FALSE(truing_state_is_operational(TRUING_STATE_READY));
    TEST_ASSERT_FALSE(truing_state_is_operational(TRUING_STATE_TERMINAL));
    TEST_ASSERT_EQUAL_STRING("REMEASURE_GAPS", truing_state_str(TRUING_STATE_REMEASURE_GAPS));
    TEST_ASSERT_EQUAL_STRING("?", truing_state_str((truing_state_t)99));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_every_runtime_parameter_is_classified);
    RUN_TEST(test_class_assignments);
    RUN_TEST(test_admissibility_matrix);
    RUN_TEST(test_state_classification);
    return UNITY_END();
}
