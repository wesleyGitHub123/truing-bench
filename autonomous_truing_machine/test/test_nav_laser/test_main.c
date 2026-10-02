/* Laser-guided roller navigation (navigation_laser): the firmware port of the alignment campaign's revolution
 * sequencer, run against a simulated wheel that has the bench wheel's 4-spoke spacing, its blocked width and
 * the recipes' stop distances (v^2/2a). The failures injected are the ones the bench saw (docs/ALIGN_CAMPAIGN.md):
 * a strike nudging a spoke into or past the beam, a missed break (a stop one spoke late), beam chatter, a
 * recipe that will not land, and the driver's microstep setting reverting. */
#include <string.h>
#include <unity.h>

#include "truing_fixtures/fixtures.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_laser.h"

#define WIDTH 66

typedef struct {
    int32_t  p;                 /* drive position, steps */
    int32_t  base;              /* spoke 0's entering edge */
    int      moving_polls;
    int      trial_polls;
    truing_laser_trial_t t;
    uint32_t edges;
    bool     rescale_next;
    bool     skip_break_next;   /* the next run misses one break: it stops one spoke late */
    bool     chatter_next;
    int32_t  stop_noise;        /* added to every stop distance */
    uint32_t starts, moves, stops, releases;
    int32_t  last_trigger_spoke;
} sim_t;

static sim_t g_sim;
static truing_laser_nav_config_t g_cfg;
static truing_machine_profile_t g_machine;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static truing_navigation_if_t g_nav;
static truing_navigation_laser_ctx_t g_ctx;

static int32_t edge(int32_t s)
{
    return g_sim.base + truing_laser_nav_offset(&g_cfg, 0, s);
}

static bool blocked_at(int32_t p)
{
    for (int32_t s = -8; s < 80; ++s) {
        if (edge(s) <= p && p < edge(s) + WIDTH) {
            return true;
        }
    }
    return false;
}

static int32_t next_entering(int32_t p, int j)
{
    for (int32_t s = -8; s < 80; ++s) {
        if (edge(s) > p && --j == 0) {
            return s;
        }
    }
    return 999;
}

static bool sim_enable(void *u, bool *rescaled)
{
    (void)u;
    *rescaled = g_sim.rescale_next;
    g_sim.rescale_next = false;
    return true;
}

static bool sim_start(void *u, const truing_align_config_t *cfg, uint32_t hz, uint32_t accel)
{
    (void)u;
    g_sim.starts++;
    int j = cfg->target_break + (g_sim.skip_break_next ? 1 : 0);
    g_sim.skip_break_next = false;
    const int32_t s = next_entering(g_sim.p, j);
    const int32_t trig = edge(s);
    const int32_t stop = trig + (int32_t)((uint64_t)hz * hz / (2u * accel)) + g_sim.stop_noise;
    memset(&g_sim.t, 0, sizeof(g_sim.t));
    g_sim.t.fired = true;
    g_sim.t.trigger_pos = trig;
    g_sim.t.stop_pos = stop;
    g_sim.t.width_used = WIDTH;
    int32_t final = stop;
    if (cfg->strategy == TRUING_ALIGN_STRAT_STOP) {
        g_sim.t.final_zone = stop < trig + WIDTH ? 0 : 1;
        g_sim.t.result = g_sim.t.final_zone == 0 ? TRUING_ALIGN_RESULT_ALIGNED : TRUING_ALIGN_RESULT_OVERSHOOT;
    } else if (cfg->strategy == TRUING_ALIGN_STRAT_REACQUIRE) {
        final = stop < trig + WIDTH ? stop : trig + WIDTH - 1;
        g_sim.t.final_zone = 0;
        g_sim.t.result = TRUING_ALIGN_RESULT_ALIGNED;
    } else {   /* REACQUIRE_CLEAR: back through the spoke until it clears on the near side, then the offset */
        final = trig - 1 + cfg->dir * cfg->offset_steps;
        g_sim.t.final_zone = -1;
        g_sim.t.result = TRUING_ALIGN_RESULT_ALIGNED;
    }
    if (g_sim.stop_noise != 0) {
        g_sim.t.result = cfg->strategy == TRUING_ALIGN_STRAT_STOP ? g_sim.t.result : TRUING_ALIGN_RESULT_OVERSHOOT;
    }
    g_sim.p = final;
    g_sim.t.final_blocked = blocked_at(final);
    g_sim.edges += 4u + (g_sim.chatter_next ? 1000u : 0u);
    g_sim.chatter_next = false;
    g_sim.last_trigger_spoke = s;
    g_sim.trial_polls = 3;
    g_sim.t.active = true;
    return true;
}

static bool sim_trial(void *u, truing_laser_trial_t *out)
{
    (void)u;
    if (g_sim.trial_polls > 0 && --g_sim.trial_polls == 0) {
        g_sim.t.active = false;
    }
    *out = g_sim.t;
    return true;
}

static bool sim_move(void *u, int32_t steps, uint32_t hz, uint32_t accel)
{
    (void)u;
    (void)hz;
    (void)accel;
    g_sim.moves++;
    g_sim.p += steps;
    g_sim.moving_polls = 2;
    return true;
}

static bool sim_moving(void *u)
{
    (void)u;
    if (g_sim.moving_polls > 0) {
        g_sim.moving_polls--;
        return true;
    }
    return false;
}

static int32_t sim_position(void *u) { (void)u; return g_sim.p; }
static bool sim_blocked(void *u) { (void)u; return blocked_at(g_sim.p); }
static uint32_t sim_edges(void *u) { (void)u; return g_sim.edges; }
static void sim_stop(void *u) { (void)u; g_sim.stops++; g_sim.t.active = false; g_sim.trial_polls = 0; }
static void sim_release(void *u) { (void)u; g_sim.releases++; }

static const truing_laser_port_t k_port = { sim_enable, sim_start, sim_trial, sim_move, sim_moving, sim_position,
                                            sim_blocked, sim_edges, sim_stop, sim_release, NULL };

static truing_nav_target_t spoke_target(uint8_t i)
{
    truing_nav_target_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TRUING_NAV_TARGET_SPOKE;
    t.index = i;
    t.station = (i % 2u) == 0u ? TRUING_STATION_ACOUSTIC_LEFT : TRUING_STATION_ACOUSTIC_RIGHT;
    return t;
}

static truing_nav_outcome_t go(uint8_t idx, truing_nav_result_t *r)
{
    const truing_nav_target_t t = spoke_target(idx);
    g_nav.request(&g_nav, &t, r);
    for (int i = 0; i < 400 && r->outcome == TRUING_NAV_IN_MOTION; ++i) {
        g_nav.poll(&g_nav, r);
    }
    return r->outcome;
}

/* The operator places spoke 0 just before the beam and confirms it at LEFT (the reference). */
static void reference(void)
{
    truing_nav_result_t r;
    g_nav.establish_reference(&g_nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION, r.wait_kind);
    g_sim.p = g_sim.base - 400;
    g_nav.note_operator_confirmation(&g_nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
}

void setUp(void)
{
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.base = 10000;
    truing_laser_nav_config_bench_2026_10_03(&g_cfg);
    truing_fixture_machine_profile(&g_machine);
    truing_fake_clock_init(&g_fc, &g_clock, 1000u);
    truing_navigation_laser_init(&g_nav, &g_ctx, g_clock, 32u, &g_machine, &g_cfg, &k_port, TRUING_SOURCE_REAL);
}
void tearDown(void) {}

static void test_spacing_names_a_trigger(void)
{
    int32_t who = 0, err = 0;
    TEST_ASSERT_EQUAL_INT32(1980 + 700 + 1965, truing_laser_nav_offset(&g_cfg, 0, 3));
    TEST_ASSERT_EQUAL_INT32(-(1980 + 700 + 1965), truing_laser_nav_offset(&g_cfg, 3, 0));
    TEST_ASSERT_TRUE(truing_laser_nav_identify(&g_cfg, edge(0), 0, edge(3) + 40, &who, &err));
    TEST_ASSERT_EQUAL_INT32(3, who);
    TEST_ASSERT_EQUAL_INT32(40, err);
    TEST_ASSERT_TRUE(truing_laser_nav_identify(&g_cfg, edge(5), 5, edge(10), &who, &err));
    TEST_ASSERT_EQUAL_INT32(10, who);
    /* half-way between two edges is nobody's */
    TEST_ASSERT_FALSE(truing_laser_nav_identify(&g_cfg, edge(0), 0, edge(3) + 1600, &who, &err));
}

/* The headline: one full revolution, every spoke found and stopped on by itself, nobody asked. */
static void test_a_clean_revolution_needs_no_operator(void)
{
    reference();
    truing_nav_result_t r;
    for (uint8_t i = 0; i < 32u; ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(TRUING_NAV_DONE, go(i, &r), "spoke");
        TEST_ASSERT_EQUAL_INT32(i, g_sim.last_trigger_spoke);
    }
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));   /* the counter wraps: spoke 0 again, one turn on */
    TEST_ASSERT_EQUAL_INT32(32, g_sim.last_trigger_spoke);
    TEST_ASSERT_EQUAL_UINT32(33u, g_ctx.trials);
    TEST_ASSERT_EQUAL_UINT32(0u, g_ctx.handovers);
    TEST_ASSERT_EQUAL_UINT32(0u, g_ctx.backups);
    truing_wheel_position_t pos;
    g_nav.query(&g_nav, &pos);
    TEST_ASSERT_TRUE(pos.sensor_confirmed);   /* named by spacing */
    TEST_ASSERT_FALSE(pos.operator_confirmed);
}

/* A strike nudged the next spoke INTO the beam: it would not be counted, and a plain run would stop one
 * spoke late. The belief moves on, the spoke is behind its recipe's stop, so the sequencer backs up. */
static void test_a_spoke_nudged_into_the_beam_is_backed_up_for(void)
{
    reference();
    truing_nav_result_t r;
    for (uint8_t i = 0; i < 3u; ++i) {
        TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(i, &r));
    }
    g_sim.p = edge(3) + 10;   /* spoke 3 pushed into the beam */
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(3, &r));
    TEST_ASSERT_EQUAL_INT32(3, g_sim.last_trigger_spoke);
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.backups);
    TEST_ASSERT_EQUAL_UINT32(0u, g_ctx.handovers);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(4, &r));
    TEST_ASSERT_EQUAL_INT32(4, g_sim.last_trigger_spoke);
}

/* A strike pushed the spoke in the beam ON past it: still behind, the next break is the next spoke. */
static void test_a_spoke_pushed_past_the_beam_needs_nothing(void)
{
    reference();
    truing_nav_result_t r;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));
    TEST_ASSERT_TRUE(blocked_at(g_sim.p));
    g_sim.p = edge(0) + WIDTH + 5;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(1, &r));
    TEST_ASSERT_EQUAL_INT32(1, g_sim.last_trigger_spoke);
    TEST_ASSERT_EQUAL_UINT32(0u, g_ctx.backups);
}

/* A missed break stops on spoke k+1. The spacing names it before anything strikes; back up, run again. */
static void test_a_stop_on_the_wrong_spoke_is_named_and_retried(void)
{
    reference();
    truing_nav_result_t r;
    for (uint8_t i = 0; i < 5u; ++i) {
        TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(i, &r));
    }
    g_sim.skip_break_next = true;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(5, &r));
    TEST_ASSERT_EQUAL_INT32(5, g_sim.last_trigger_spoke);
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.misnamed);
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.backups);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(6, &r));
    TEST_ASSERT_EQUAL_INT32(6, g_sim.last_trigger_spoke);
}

static void test_beam_chatter_discards_the_run(void)
{
    reference();
    truing_nav_result_t r;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));
    g_sim.chatter_next = true;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(1, &r));
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.chatter);
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.backups);
    TEST_ASSERT_EQUAL_INT32(1, g_sim.last_trigger_spoke);
}

/* A recipe that will not land is tried max_tries times, then the operator positions that spoke with the
 * jog; the next spoke is automatic again. */
static void test_bounded_retries_then_the_operator_then_automatic_again(void)
{
    reference();
    truing_nav_result_t r;
    for (uint8_t i = 0; i < 2u; ++i) {
        TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(i, &r));
    }
    const uint32_t trials0 = g_ctx.trials;
    g_sim.stop_noise = 60;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, go(2, &r));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_SPOKE, r.wait_kind);
    TEST_ASSERT_EQUAL_UINT8(2u, r.target.index);
    TEST_ASSERT_EQUAL_UINT32(3u, g_ctx.trials - trials0);
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.handovers);
    TEST_ASSERT_EQUAL_UINT32(1u, g_sim.releases);   /* the roller let go so the operator can turn the wheel */
    g_sim.stop_noise = 0;
    g_sim.p = edge(2) + 30;   /* the operator jogs spoke 2 to the strike point */
    g_nav.note_operator_confirmation(&g_nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    truing_wheel_position_t pos;
    g_nav.query(&g_nav, &pos);
    TEST_ASSERT_TRUE(pos.operator_confirmed);
    TEST_ASSERT_FALSE(pos.sensor_confirmed);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(3, &r));
    TEST_ASSERT_EQUAL_INT32(3, g_sim.last_trigger_spoke);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(4, &r));
    TEST_ASSERT_EQUAL_INT32(4, g_sim.last_trigger_spoke);
}

/* Incident 1: the driver reset to its pin-strapped microsteps and the enable repaired it. Every held step
 * position is in another scale: the spacing anchor is dropped and re-taken, the spoke count carries on. */
static void test_a_repaired_microstep_setting_re_anchors(void)
{
    reference();
    truing_nav_result_t r;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(1, &r));
    g_sim.rescale_next = true;
    g_sim.base += 5000;   /* positions jump: the old anchor would name the next trigger wrongly */
    g_sim.p += 5000;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(2, &r));
    TEST_ASSERT_EQUAL_INT32(2, g_sim.last_trigger_spoke);
    TEST_ASSERT_EQUAL_UINT32(1u, g_ctx.rescales);
    TEST_ASSERT_EQUAL_UINT32(0u, g_ctx.handovers);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(3, &r));
    TEST_ASSERT_EQUAL_INT32(3, g_sim.last_trigger_spoke);
}

/* ABORT mid-run: the port stops the drive, and the wheel position is no longer vouched for. */
static void test_stop_mid_run_stops_the_drive_and_forgets_the_position(void)
{
    reference();
    truing_nav_result_t r;
    const truing_nav_target_t t = spoke_target(0);
    g_nav.request(&g_nav, &t, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_IN_MOTION, r.outcome);
    g_nav.stop(&g_nav);
    TEST_ASSERT_EQUAL_UINT32(1u, g_sim.stops);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, go(0, &r));
}

/* The likeliest mistake at the start: spoke 0 confirmed IN the beam instead of just before it. A run from
 * there would stop on spoke 1 and strike it as spoke 0. It must go back to the operator, unstruck. */
static void test_spoke_0_placed_in_the_beam_is_not_struck_as_spoke_1(void)
{
    truing_nav_result_t r;
    g_nav.establish_reference(&g_nav, &r);
    g_sim.p = g_sim.base + 20;   /* in the beam */
    g_nav.note_operator_confirmation(&g_nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, go(0, &r));
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_SPOKE, r.wait_kind);
    TEST_ASSERT_EQUAL_UINT32(0u, g_sim.starts);
    /* the operator leaves spoke 0 in the beam and confirms: spoke 1 is then automatic */
    g_nav.note_operator_confirmation(&g_nav, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, r.outcome);
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(1, &r));
    TEST_ASSERT_EQUAL_INT32(1, g_sim.last_trigger_spoke);
}

/* A blocked width far from ~65 means the step scale or the beam is off: no back-up computed in that scale. */
static void test_a_width_off_its_value_goes_to_the_operator(void)
{
    reference();
    truing_nav_result_t r;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));
    const truing_nav_target_t t = spoke_target(1);
    g_nav.request(&g_nav, &t, &r);
    g_sim.t.width_used = 33;   /* the halved scale of incident 1 */
    for (int i = 0; i < 400 && r.outcome == TRUING_NAV_IN_MOTION; ++i) {
        g_nav.poll(&g_nav, &r);
    }
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    TEST_ASSERT_EQUAL_UINT32(0u, g_ctx.backups);
}

/* sensor_confirmed means the spacing NAMED the spoke: not for the first trigger after a placement. */
static void test_only_a_named_spoke_is_sensor_confirmed(void)
{
    reference();
    truing_nav_result_t r;
    truing_wheel_position_t pos;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));
    g_nav.query(&g_nav, &pos);
    TEST_ASSERT_FALSE(pos.sensor_confirmed);   /* trusted, not named */
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(1, &r));
    g_nav.query(&g_nav, &pos);
    TEST_ASSERT_TRUE(pos.sensor_confirmed);
}

/* A run that never saw its break (a dead or blocked beam): back up and retry, then the operator. */
static void test_a_missed_laser_is_retried_then_handed_over(void)
{
    reference();
    truing_nav_result_t r;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_DONE, go(0, &r));
    const truing_nav_target_t t = spoke_target(1);
    g_nav.request(&g_nav, &t, &r);
    int misses = 0;
    for (int i = 0; i < 2000 && r.outcome == TRUING_NAV_IN_MOTION; ++i) {
        if (g_sim.t.active && g_sim.trial_polls == 1) {
            g_sim.t.fired = false;
            g_sim.t.result = TRUING_ALIGN_RESULT_LASER_MISSED;
            misses++;
        }
        g_nav.poll(&g_nav, &r);
    }
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, r.outcome);
    TEST_ASSERT_EQUAL_INT(3, misses);
    TEST_ASSERT_EQUAL_UINT32(2u, g_ctx.backups);
}

/* Before the reference nothing is known: the operator is asked. */
static void test_no_reference_means_the_operator_positions(void)
{
    truing_nav_result_t r;
    TEST_ASSERT_EQUAL_INT(TRUING_NAV_PENDING_OPERATOR, go(0, &r));
    TEST_ASSERT_EQUAL_UINT32(0u, g_sim.starts);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_spacing_names_a_trigger);
    RUN_TEST(test_a_clean_revolution_needs_no_operator);
    RUN_TEST(test_a_spoke_nudged_into_the_beam_is_backed_up_for);
    RUN_TEST(test_a_spoke_pushed_past_the_beam_needs_nothing);
    RUN_TEST(test_a_stop_on_the_wrong_spoke_is_named_and_retried);
    RUN_TEST(test_beam_chatter_discards_the_run);
    RUN_TEST(test_bounded_retries_then_the_operator_then_automatic_again);
    RUN_TEST(test_a_repaired_microstep_setting_re_anchors);
    RUN_TEST(test_stop_mid_run_stops_the_drive_and_forgets_the_position);
    RUN_TEST(test_no_reference_means_the_operator_positions);
    RUN_TEST(test_spoke_0_placed_in_the_beam_is_not_struck_as_spoke_1);
    RUN_TEST(test_a_width_off_its_value_goes_to_the_operator);
    RUN_TEST(test_only_a_named_spoke_is_sensor_confirmed);
    RUN_TEST(test_a_missed_laser_is_retried_then_handed_over);
    return UNITY_END();
}
