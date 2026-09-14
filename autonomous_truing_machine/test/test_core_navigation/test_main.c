/* Wheel Navigation / Positioning (SPEC §10A): geometry relation, manual (C2) implementation,
 * synthetic automated implementation, wheel-drive contract. */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "truing/wheel_geometry.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/navigation_synthetic.h"
#include "truing_hal/wheel_drive_if.h"

static truing_machine_profile_t g_machine;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;

void setUp(void)
{
    truing_fixture_machine_profile(&g_machine);   /* acoustic 0, runout π/2, adjustment π */
    truing_fake_clock_init(&g_fc, &g_clock, 5000u);
}
void tearDown(void) {}

static truing_nav_target_t spoke_at(uint8_t i, truing_station_id_t s)
{
    truing_nav_target_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TRUING_NAV_TARGET_SPOKE;
    t.index = i;
    t.station = s;
    return t;
}

/* ---- geometry (the ONE wheel/station relation) ------------------------------------- */
static void test_geometry_relation(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f * TRUING_PI / 2.0f, truing_wrap_angle(-TRUING_PI / 2.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, truing_wrap_angle(TRUING_TWO_PI));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, truing_wrap_angle(1.0f + 2.0f * TRUING_TWO_PI));
    TEST_ASSERT_TRUE(isnan(truing_wrap_angle(NAN)));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -TRUING_PI / 2.0f, truing_shortest_delta(0.0f, 3.0f * TRUING_PI / 2.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f, truing_shortest_delta(3.0f * TRUING_PI / 2.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f, truing_circular_distance(0.0f, 3.0f * TRUING_PI / 2.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f, truing_spoke_angle(32u, 8u));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f, truing_spoke_angle(36u, 9u));
    TEST_ASSERT_TRUE(isnan(truing_spoke_angle(32u, 32u)));
    TEST_ASSERT_TRUE(isnan(truing_rim_index_angle(0u, 0u)));
    /* Feature at θ = π/2 brought to a station at φ = 0 needs R = 3π/2; then it sits at machine angle 0. */
    const float r = truing_rotation_for_feature_at(TRUING_PI / 2.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f * TRUING_PI / 2.0f, r);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, truing_machine_angle_of_feature(TRUING_PI / 2.0f, r));
    /* Spoke 0 at a station at φ: R = φ. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI, truing_rotation_for_feature_at(0.0f, TRUING_PI));
}

static void test_target_resolution_and_prompt_mapping(void)
{
    truing_nav_target_t t = spoke_at(4u, TRUING_STATION_RUNOUT);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 4.0f, truing_nav_target_wheel_angle(&t, 32u, 32u));
    t.kind = TRUING_NAV_TARGET_RIM_INDEX;
    t.index = 9u;
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f, truing_nav_target_wheel_angle(&t, 36u, 36u));
    t.kind = TRUING_NAV_TARGET_RIM_ANGLE;
    t.rim_angle_rad = 1.234f;
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.234f, truing_nav_target_wheel_angle(&t, 32u, 32u));
    t.rim_angle_rad = TRUING_TWO_PI;
    TEST_ASSERT_TRUE(isnan(truing_nav_target_wheel_angle(&t, 32u, 32u)));
    t.kind = TRUING_NAV_TARGET_UNSET;
    TEST_ASSERT_TRUE(isnan(truing_nav_target_wheel_angle(&t, 32u, 32u)));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_SPOKE, truing_nav_target_wait_kind(TRUING_NAV_TARGET_SPOKE));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_RIM_INDEX, truing_nav_target_wait_kind(TRUING_NAV_TARGET_RIM_INDEX));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_RIM_ANGLE, truing_nav_target_wait_kind(TRUING_NAV_TARGET_RIM_ANGLE));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_KIND_UNSET, truing_nav_target_wait_kind(TRUING_NAV_TARGET_UNSET));
}

/* ---- manual implementation (Stage 1) ------------------------------------------- */
static void test_manual_reference_then_positioning_cycle(void)
{
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t ctx;
    truing_navigation_manual_init(&nav, &ctx, g_clock, 32u, 32u, &g_machine);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_REAL, nav.source_impl);

    truing_wheel_position_t pos;
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, pos.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_WHEEL_REFERENCE_LOST, pos.reason);
    TEST_ASSERT_FALSE(pos.reference_established);

    /* SPEC §6.5: reference = spoke 0 at the reference station, via operator confirmation. */
    truing_nav_result_t r;
    truing_navigation_establish_reference(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION, r.wait_kind);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_ACOUSTIC_LEFT, r.target.station);
    truing_wait_prompt_t prompt;
    TEST_ASSERT_TRUE(truing_nav_result_to_prompt(&r, &prompt));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION, prompt.kind);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_ACOUSTIC_LEFT, prompt.station);
    TEST_ASSERT_EQUAL_UINT8(0u, prompt.target_index);
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    truing_fake_clock_advance(&g_fc, 100u);
    truing_navigation_confirm(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, pos.status);
    TEST_ASSERT_TRUE(pos.reference_established);
    TEST_ASSERT_TRUE(pos.operator_confirmed);
    TEST_ASSERT_FALSE(pos.sensor_confirmed);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, pos.rotation_rad);
    TEST_ASSERT_EQUAL_UINT32(5100u, pos.timestamp_ms);

    /* Spoke 4 (θ = π/4) to the runout station (φ = π/2): R = π/4. */
    truing_nav_target_t t = spoke_at(4u, TRUING_STATION_RUNOUT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_SPOKE, r.wait_kind);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_RUNOUT, r.target.station);
    TEST_ASSERT_TRUE(truing_nav_result_to_prompt(&r, &prompt));
    TEST_ASSERT_EQUAL_UINT8(4u, prompt.target_index);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_RUNOUT, prompt.station);
    truing_navigation_confirm(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 4.0f, pos.rotation_rad);
    /* Consistency: spoke 4 now sits at the runout station's machine angle. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f,
                             truing_machine_angle_of_feature(truing_spoke_angle(32u, 4u), pos.rotation_rad));

    /* Rim index 4 to the acoustic station (φ = 0): R = 7π/4. Same feature, different station,
     * different rotation — the station is not a universal constant. */
    t.kind = TRUING_NAV_TARGET_RIM_INDEX;
    t.station = TRUING_STATION_ACOUSTIC_LEFT;
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_RIM_INDEX, r.wait_kind);
    truing_navigation_confirm(&nav, &r);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 7.0f * TRUING_PI / 4.0f, pos.rotation_rad);

    /* Arbitrary rim angle to the adjustment station (φ = π). */
    t.kind = TRUING_NAV_TARGET_RIM_ANGLE;
    t.rim_angle_rad = 1.0f;
    t.station = TRUING_STATION_ADJUSTMENT;
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_RIM_ANGLE, r.wait_kind);
    TEST_ASSERT_TRUE(truing_nav_result_to_prompt(&r, &prompt));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, prompt.target_angle_rad);
    truing_navigation_confirm(&nav, &r);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI - 1.0f, pos.rotation_rad);
    TEST_ASSERT_EQUAL_UINT32(4u, ctx.confirmations);
}

static void test_manual_refusals(void)
{
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t ctx;
    truing_navigation_manual_init(&nav, &ctx, g_clock, 32u, 32u, &g_machine);
    truing_nav_result_t r;
    /* Station absent from the machine profile. */
    truing_nav_target_t t = spoke_at(3u, TRUING_STATION_REFERENCE);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, r.reason);
    /* Feature out of range. */
    t = spoke_at(40u, TRUING_STATION_ACOUSTIC_LEFT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_VALUE_OUT_OF_RANGE, r.reason);
    /* Confirmation with nothing pending is stale. */
    truing_navigation_confirm(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_STALE_INTENT, r.reason);
    /* A refused request does not become pending. */
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_IDLE, r.outcome);
    /* stop() drops a pending request without touching position knowledge. */
    t = spoke_at(3u, TRUING_STATION_ACOUSTIC_LEFT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    truing_navigation_stop(&nav);
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_IDLE, r.outcome);
    /* A null implementation never pretends. */
    truing_navigation_request(NULL, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, r.reason);
    truing_wheel_position_t pos;
    truing_navigation_query(NULL, &pos);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, pos.status);
}

/* ---- synthetic automated implementation ------------------------------------------- */
static void test_synthetic_positions_through_actuator_coordinates(void)
{
    truing_wheel_drive_if_t drive;
    truing_wheel_drive_fake_ctx_t dctx;
    truing_wheel_drive_fake_init(&drive, &dctx);
    truing_reason_t reason = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, truing_wheel_drive_enable(&drive, true, &reason));

    truing_navigation_if_t nav;
    truing_navigation_synthetic_ctx_t ctx;
    /* 3200 steps per wheel revolution is a TEST calibration knob, not a system constant. */
    truing_navigation_synthetic_init(&nav, &ctx, g_clock, 32u, 32u, &g_machine, &drive, 3200.0f, 0u);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, nav.source_impl);

    truing_nav_result_t r;
    truing_nav_target_t t = spoke_at(4u, TRUING_STATION_RUNOUT);
    /* No reference yet: refused, never guessed. */
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_WHEEL_REFERENCE_LOST, r.reason);

    truing_navigation_establish_reference(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    truing_wheel_position_t pos;
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_TRUE(pos.reference_established);
    TEST_ASSERT_TRUE(pos.sensor_confirmed);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, pos.rotation_rad);

    /* Spoke 4 -> runout: ΔR = +π/4 -> 400 of 3200 steps. */
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    TEST_ASSERT_EQUAL_INT32(400, dctx.last_move_steps);
    TEST_ASSERT_EQUAL_INT32(400, truing_wheel_drive_position_steps(&drive));
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 4.0f, pos.rotation_rad);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, pos.rotation_rad, truing_navigation_synthetic_true_rotation(&ctx));
    TEST_ASSERT_FALSE(pos.sensor_confirmed);   /* commanded motion only: not corroborated */

    /* Shortest path: back to spoke 0 at acoustic is −π/4, i.e. −400 steps, not +2800. */
    t = spoke_at(0u, TRUING_STATION_ACOUSTIC_LEFT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT32(-400, dctx.last_move_steps);
    truing_navigation_query(&nav, &pos);
    /* Angles compare circularly: 0 and 2π are the same rotation state. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, truing_circular_distance(0.0f, pos.rotation_rad));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f,
                             truing_circular_distance(pos.rotation_rad, truing_navigation_synthetic_true_rotation(&ctx)));
}

static void test_synthetic_slip_separates_believed_from_true(void)
{
    truing_navigation_if_t nav;
    truing_navigation_synthetic_ctx_t ctx;
    truing_navigation_synthetic_init(&nav, &ctx, g_clock, 32u, 32u, &g_machine, NULL, 0.0f, 0u);
    truing_nav_result_t r;
    truing_navigation_establish_reference(&nav, &r);
    ctx.scripted_slip_rad = 0.1f;   /* the friction drive under-rotates the wheel */
    truing_nav_target_t t = spoke_at(8u, TRUING_STATION_ADJUSTMENT);   /* θ=π/2 -> φ=π: R'=π/2 */
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    truing_wheel_position_t pos;
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f, pos.rotation_rad);                          /* believed */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f + 0.1f, truing_navigation_synthetic_true_rotation(&ctx)); /* true */
    /* The implementation is honest about not being sensor-corroborated. */
    TEST_ASSERT_FALSE(pos.sensor_confirmed);
    /* Re-establishing the reference re-anchors both. */
    truing_navigation_establish_reference(&nav, &r);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f,
                             truing_circular_distance(truing_navigation_synthetic_true_rotation(&ctx), pos.rotation_rad));
}

static void test_synthetic_in_motion_polling_stop_and_fault(void)
{
    truing_navigation_if_t nav;
    truing_navigation_synthetic_ctx_t ctx;
    truing_navigation_synthetic_init(&nav, &ctx, g_clock, 32u, 32u, &g_machine, NULL, 0.0f, 2u);
    truing_nav_result_t r;
    truing_navigation_establish_reference(&nav, &r);
    truing_nav_target_t t = spoke_at(2u, TRUING_STATION_RUNOUT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_IN_MOTION, r.outcome);
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_IN_MOTION, r.outcome);
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    truing_wheel_position_t pos;
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TRUING_PI / 2.0f - truing_spoke_angle(32u, 2u), pos.rotation_rad);

    /* ABORT mid-motion: controlled stop, reference no longer vouched for (SPEC §10A.7). */
    t = spoke_at(20u, TRUING_STATION_ACOUSTIC_LEFT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_IN_MOTION, r.outcome);
    truing_navigation_stop(&nav);
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_FAULT, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_WHEEL_REFERENCE_LOST, r.reason);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, pos.status);
    TEST_ASSERT_FALSE(pos.reference_established);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_WHEEL_REFERENCE_LOST, r.reason);
    /* Recovery is re-establishing the reference. */
    truing_navigation_establish_reference(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);

    /* Scripted fault during a move. */
    ctx.fault_next = true;
    truing_navigation_request(&nav, &t, &r);
    truing_navigation_poll(&nav, &r);
    truing_navigation_poll(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_FAULT, r.outcome);
    truing_navigation_query(&nav, &pos);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_WHEEL_REFERENCE_LOST, pos.reason);
    /* Operator confirmation is meaningless to an automated implementation. */
    truing_navigation_confirm(&nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
}

static void test_synthetic_refuses_unknown_calibration(void)
{
    truing_wheel_drive_if_t drive;
    truing_wheel_drive_fake_ctx_t dctx;
    truing_wheel_drive_fake_init(&drive, &dctx);
    truing_navigation_if_t nav;
    truing_navigation_synthetic_ctx_t ctx;
    /* Drive attached but steps-per-revolution unknown (0): must refuse, never assume a ratio. */
    truing_navigation_synthetic_init(&nav, &ctx, g_clock, 32u, 32u, &g_machine, &drive, 0.0f, 0u);
    truing_nav_result_t r;
    truing_navigation_establish_reference(&nav, &r);
    truing_nav_target_t t = spoke_at(4u, TRUING_STATION_RUNOUT);
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, r.reason);
    TEST_ASSERT_EQUAL_UINT32(0u, dctx.moves);
    /* Drive present but disabled: the drive's refusal propagates. */
    ctx.steps_per_wheel_rev = 3200.0f;
    truing_navigation_request(&nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_REFUSED, r.outcome);
    TEST_ASSERT_EQUAL_UINT32(0u, dctx.moves);
}

/* ---- wheel-drive contract ------------------------------------------------------------ */
static void test_wheel_drive_stub_and_fake(void)
{
    truing_wheel_drive_if_t d;
    truing_wheel_drive_stub_ctx_t sctx;
    truing_reason_t reason = TRUING_REASON_NONE;
    truing_wheel_drive_stub_init(&d, &sctx);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_wheel_drive_enable(&d, true, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, reason);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_wheel_drive_move_relative(&d, 10, &reason));
    TEST_ASSERT_FALSE(truing_wheel_drive_is_moving(&d));
    TEST_ASSERT_EQUAL_INT32(0, truing_wheel_drive_position_steps(&d));
    TEST_ASSERT_EQUAL_UINT32(2u, sctx.calls);

    truing_wheel_drive_fake_ctx_t fctx;
    truing_wheel_drive_fake_init(&d, &fctx);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_wheel_drive_move_relative(&d, 10, &reason));   /* disabled */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, truing_wheel_drive_enable(&d, true, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, truing_wheel_drive_move_relative(&d, 10, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, truing_wheel_drive_move_relative(&d, -3, &reason));
    TEST_ASSERT_EQUAL_INT32(7, truing_wheel_drive_position_steps(&d));
    truing_wheel_drive_stop(&d);
    TEST_ASSERT_EQUAL_UINT32(1u, fctx.stops);
    /* Null-safe wrappers. */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_wheel_drive_enable(NULL, true, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, reason);
    truing_wheel_drive_stop(NULL);
    TEST_ASSERT_EQUAL_STRING("PENDING_OPERATOR", truing_nav_outcome_str(TRUING_NAV_PENDING_OPERATOR));
    TEST_ASSERT_EQUAL_STRING("rim_angle", truing_nav_target_kind_str(TRUING_NAV_TARGET_RIM_ANGLE));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_geometry_relation);
    RUN_TEST(test_target_resolution_and_prompt_mapping);
    RUN_TEST(test_manual_reference_then_positioning_cycle);
    RUN_TEST(test_manual_refusals);
    RUN_TEST(test_synthetic_positions_through_actuator_coordinates);
    RUN_TEST(test_synthetic_slip_separates_believed_from_true);
    RUN_TEST(test_synthetic_in_motion_polling_stop_and_fault);
    RUN_TEST(test_synthetic_refuses_unknown_calibration);
    RUN_TEST(test_wheel_drive_stub_and_fake);
    return UNITY_END();
}
