/* Configuration structures and validation (SPEC §11.1–§11.3.1, §11.6, §8.9, §9.3). */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "truing/config.h"
#include "truing_fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

static void test_fixture_wheels_validate(void)
{
    truing_wheel_class_config_t w;
    const char *field = NULL;
    truing_fixture_wheel_class_sym32(&w);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_wheel_class_config_check(&w, &field));
    truing_fixture_wheel_class_asym36(&w);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_wheel_class_config_check(&w, &field));
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_NULL, truing_wheel_class_config_check(NULL, &field));
}

static void test_bracing_angles_from_geometry(void)
{
    truing_wheel_class_config_t w;
    float a = 0.0f, b = 0.0f;
    truing_fixture_wheel_class_sym32(&w);
    TEST_ASSERT_TRUE(truing_wheel_class_bracing_angles(&w, &a, &b));
    /* atan2(35, 311 - 22.5) = atan(0.121317) = 0.12073 rad, hand-derived. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.12073f, a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.12073f, b);
    truing_fixture_wheel_class_asym36(&w);
    TEST_ASSERT_TRUE(truing_wheel_class_bracing_angles(&w, &a, &b));
    /* atan2(20, 288.5) = 0.06920 rad; Side B unchanged. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.06920f, a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.12073f, b);
}

static void test_symmetry_claim_must_match_geometry(void)
{
    /* SPEC §11.1: asymmetric=false REQUIRES the bracing angles to agree within tolerance. */
    truing_wheel_class_config_t w;
    const char *field = NULL;
    truing_fixture_wheel_class_sym32(&w);
    w.hub_side_a.flange_offset_mm = 20.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_SYMMETRY_CLAIM_CONTRADICTS_GEOMETRY, truing_wheel_class_config_check(&w, &field));
    TEST_ASSERT_EQUAL_STRING("asymmetric", field);
    /* Declaring it asymmetric is always safe. */
    w.asymmetric = true;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_wheel_class_config_check(&w, &field));
}

static void test_wheel_class_field_rules(void)
{
    truing_wheel_class_config_t w;
    const char *field = NULL;
    truing_fixture_wheel_class_sym32(&w);
    w.n_spokes = 30u;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_N_SPOKES_UNSUPPORTED, truing_wheel_class_config_check(&w, &field));
    truing_fixture_wheel_class_sym32(&w);
    w.expected_influence_fingerprint.set = false;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_wheel_class_config_check(&w, &field));
    TEST_ASSERT_EQUAL_STRING("expected_influence_fingerprint", field);
    truing_fixture_wheel_class_sym32(&w);
    w.side_a_basis = TRUING_SIDE_A_BASIS_UNSET;   /* SPEC §6.4.1: must be declared */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_wheel_class_config_check(&w, &field));
    truing_fixture_wheel_class_sym32(&w);
    w.indexing_origin.lead_trail = TRUING_LEAD_TRAIL_UNSET;   /* SPEC §6.5 */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_wheel_class_config_check(&w, &field));
    truing_fixture_wheel_class_sym32(&w);
    w.c_side_b_n_per_rev = 0.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_NOT_POSITIVE, truing_wheel_class_config_check(&w, &field));
    TEST_ASSERT_EQUAL_STRING("c_side_b_n_per_rev", field);
    truing_fixture_wheel_class_sym32(&w);
    w.compatible_model_assumptions.spoke_diameter_mm = 2.34f;   /* disagrees with the wheel's own gauge */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_wheel_class_config_check(&w, &field));
    truing_fixture_wheel_class_sym32(&w);
    w.rim_diameter_mm = NAN;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_NOT_FINITE, truing_wheel_class_config_check(&w, &field));
}

static void test_solver_config_rules(void)
{
    truing_solver_config_t s;
    const char *field = NULL;
    truing_fixture_solver_config(&s, 32u);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_solver_config_check(&s, &field));
    s.tol_lateral_mm = 0.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_NOT_POSITIVE, truing_solver_config_check(&s, &field));
    TEST_ASSERT_EQUAL_STRING("tol_lateral_mm", field);
    truing_fixture_solver_config(&s, 32u);
    s.rank_tolerance = 1.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_solver_config_check(&s, &field));
    truing_fixture_solver_config(&s, 32u);
    s.tension_model_profile_id = 0u;   /* SPEC §9.2: no model selected is a configuration error */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_solver_config_check(&s, &field));
    TEST_ASSERT_EQUAL_STRING("tension_model_profile_id", field);
    truing_fixture_solver_config(&s, 32u);
    s.tol_angular_rad = 0.0f;   /* SPEC §6.5: configuration, not zero */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_NOT_POSITIVE, truing_solver_config_check(&s, &field));
    truing_fixture_solver_config(&s, 32u);
    s.max_cycles = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_solver_config_check(&s, &field));
    truing_fixture_solver_config(&s, 32u);
    s.max_condition_number = 1.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_solver_config_check(&s, &field));
    truing_fixture_solver_config(&s, 32u);
    s.tol_tension_cv = 1.5f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_solver_config_check(&s, &field));
}

static void test_pair_rule_rim_angles_equal_spokes(void)
{
    truing_wheel_class_config_t w;
    truing_solver_config_t s;
    const char *field = NULL;
    truing_fixture_wheel_class_sym32(&w);
    truing_fixture_solver_config(&s, 32u);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_config_check_pair(&w, &s, &field));
    truing_fixture_solver_config(&s, 36u);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_RIM_ANGLES_NOT_EQUAL_SPOKES, truing_config_check_pair(&w, &s, &field));
    truing_fixture_wheel_class_asym36(&w);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_config_check_pair(&w, &s, &field));
}

static void test_chain_profile_enforces_fixed_audio_format(void)
{
    truing_chain_profile_t c;
    const char *field = NULL;
    truing_fixture_chain_profile_inmp441(&c);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_chain_profile_check(&c, &field));
    c.sample_rate_hz = 44100u;   /* SPEC §14.4: rejected, never resampled */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_AUDIO_FORMAT, truing_chain_profile_check(&c, &field));
    truing_fixture_chain_profile_inmp441(&c);
    c.bit_depth = 16u;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_AUDIO_FORMAT, truing_chain_profile_check(&c, &field));
    truing_fixture_chain_profile_inmp441(&c);
    c.f1_band_lo_hz = 700.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_BAND, truing_chain_profile_check(&c, &field));
    truing_fixture_chain_profile_inmp441(&c);
    c.transducer = TRUING_TRANSDUCER_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_chain_profile_check(&c, &field));
}

static void test_tension_model_profile_completeness(void)
{
    truing_tension_model_profile_t p;
    uint32_t missing = 0u;
    const char *field = NULL;
    truing_fixture_tension_model_profile_complete(&p);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_tension_model_profile_check(&p, &missing, &field));
    TEST_ASSERT_EQUAL_UINT32(0u, missing);

    /* SPEC §11.3.1: unestablished required parameter -> CALIBRATION_MISSING, never a default. */
    truing_fixture_tension_model_profile_incomplete(&p);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_CALIBRATION_MISSING, truing_tension_model_profile_check(&p, &missing, &field));
    TEST_ASSERT_EQUAL_UINT32(TRUING_TMP_FIELD_L_EFF, missing);

    /* A model that needs no L_eff is complete without it. */
    truing_fixture_tension_model_profile_incomplete(&p);
    p.model_name = TRUING_TENSION_MODEL_HIGHER_MODE;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_tension_model_profile_check(&p, &missing, &field));

    /* Empirical needs its coefficients, which the fixture leaves unestablished. */
    truing_fixture_tension_model_profile_complete(&p);
    p.model_name = TRUING_TENSION_MODEL_EMPIRICAL;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_CALIBRATION_MISSING, truing_tension_model_profile_check(&p, &missing, &field));
    TEST_ASSERT_EQUAL_UINT32(TRUING_TMP_FIELD_EMPIRICAL_A | TRUING_TMP_FIELD_EMPIRICAL_N, missing);

    truing_fixture_tension_model_profile_complete(&p);
    p.model_name = TRUING_TENSION_MODEL_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_MODEL_UNSET, truing_tension_model_profile_check(&p, &missing, &field));

    TEST_ASSERT_EQUAL_UINT32(TRUING_TMP_FIELD_L_EFF | TRUING_TMP_FIELD_LINEAR_DENSITY | TRUING_TMP_FIELD_BENDING_STIFFNESS,
                             truing_tension_model_required_fields(TRUING_TENSION_MODEL_STIFFNESS_CORRECTED));
}

static void test_tension_model_profile_compatibility(void)
{
    truing_wheel_class_config_t w;
    truing_tension_model_profile_t p;
    const char *field = NULL;
    truing_fixture_wheel_class_sym32(&w);
    truing_fixture_tension_model_profile_complete(&p);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_tension_model_profile_compatible(&p, &w, &field));
    truing_fixture_tension_model_profile_incompatible(&p);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_MODEL_INCOMPATIBLE, truing_tension_model_profile_compatible(&p, &w, &field));
    TEST_ASSERT_EQUAL_STRING("assumptions.spoke_diameter_mm", field);
}

static void test_machine_profile_station_geometry(void)
{
    truing_machine_profile_t m;
    const char *field = NULL;
    truing_fixture_machine_profile(&m);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_machine_profile_check(&m, &field));
    TEST_ASSERT_NOT_NULL(truing_machine_profile_station(&m, TRUING_STATION_RUNOUT));
    TEST_ASSERT_NULL(truing_machine_profile_station(&m, TRUING_STATION_REFERENCE));
    TEST_ASSERT_NULL(truing_machine_profile_station(&m, TRUING_STATION_UNSET));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, TRUING_PI / 2.0f, truing_machine_profile_station(&m, TRUING_STATION_RUNOUT)->angle_rad);

    /* Every workflow station must be declared, even if it shares an angle. */
    truing_fixture_machine_profile(&m);
    m.stations[TRUING_STATION_RUNOUT].present = false;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_machine_profile_check(&m, &field));
    TEST_ASSERT_EQUAL_STRING("runout", field);
    /* Coincident stations are legal (SPEC §10A.3). */
    truing_fixture_machine_profile(&m);
    m.stations[TRUING_STATION_RUNOUT].angle_rad = m.stations[TRUING_STATION_ACOUSTIC].angle_rad;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_machine_profile_check(&m, &field));
    /* The reference station must exist. */
    truing_fixture_machine_profile(&m);
    m.reference_station = TRUING_STATION_REFERENCE;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_machine_profile_check(&m, &field));
    TEST_ASSERT_EQUAL_STRING("reference_station", field);
    /* Angles are machine-frame angles in [0, 2π). */
    truing_fixture_machine_profile(&m);
    m.stations[TRUING_STATION_ADJUSTMENT].angle_rad = TRUING_TWO_PI;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_machine_profile_check(&m, &field));
    truing_fixture_machine_profile(&m);
    m.profile_id = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_UNSET, truing_machine_profile_check(&m, &field));
    TEST_ASSERT_EQUAL_STRING("adjustment", truing_station_str(TRUING_STATION_ADJUSTMENT));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_fixture_wheels_validate);
    RUN_TEST(test_bracing_angles_from_geometry);
    RUN_TEST(test_symmetry_claim_must_match_geometry);
    RUN_TEST(test_wheel_class_field_rules);
    RUN_TEST(test_solver_config_rules);
    RUN_TEST(test_pair_rule_rim_angles_equal_spokes);
    RUN_TEST(test_chain_profile_enforces_fixed_audio_format);
    RUN_TEST(test_tension_model_profile_completeness);
    RUN_TEST(test_tension_model_profile_compatibility);
    RUN_TEST(test_machine_profile_station_geometry);
    return UNITY_END();
}
