/* Wheel-drive building blocks that need no hardware: the trapezoidal step ramp that times the
 * STEP pulses, and TMC2209 UART framing (SPEC §10A.6, §10A.8). The target only adds a timer
 * that emits one pulse per ramp step and a UART that moves the bytes. */
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing_hal/beam_align.h"
#include "truing_hal/beam_stop.h"
#include "truing_hal/spoke_sensor_if.h"
#include "truing_hal/step_ramp.h"
#include "truing_hal/tmc2209_frame.h"

void setUp(void) {}
void tearDown(void) {}

/* A 1 MHz timer; the rate and acceleration are arbitrary TEST values, not a motor's. */
static const truing_ramp_profile_t k_prof = { 1000000u, 4000u, 8000u };

typedef struct {
    uint32_t pulses;
    uint32_t after_stop;   /* pulses emitted after the stop request */
    double   seconds;
    uint32_t min_delay, max_delay;
    uint32_t delays[4096]; /* first intervals, for shape checks */
    uint32_t n_delays;
} run_t;

/* Drive a planned ramp the way the timer ISR does. `stop_at` > 0 requests a controlled stop
 * right after that many pulses. */
static void run_ramp(truing_ramp_t *r, uint32_t tick_hz, uint32_t stop_at, run_t *out)
{
    memset(out, 0, sizeof(*out));
    out->min_delay = UINT32_MAX;
    uint32_t d = (uint32_t)r->delay;
    bool stopped = false;
    for (;;) {
        out->seconds += (double)d / (double)tick_hz;
        out->pulses++;
        if (stopped) {
            out->after_stop++;
        }
        if (stop_at != 0u && out->pulses == stop_at) {
            truing_ramp_request_stop(r);
            stopped = true;
        }
        const uint32_t next = truing_ramp_step(r);
        if (next == 0u) {
            break;
        }
        if (next < out->min_delay) {
            out->min_delay = next;
        }
        if (next > out->max_delay) {
            out->max_delay = next;
        }
        if (out->n_delays < sizeof(out->delays) / sizeof(out->delays[0])) {
            out->delays[out->n_delays++] = next;
        }
        d = next;
    }
}

static run_t g_run;

/* ---- step ramp --------------------------------------------------------------------- */
static void test_ramp_refuses_what_it_cannot_time(void)
{
    truing_ramp_t r;
    TEST_ASSERT_FALSE(truing_ramp_plan(&r, &k_prof, 0u));
    TEST_ASSERT_FALSE(truing_ramp_active(&r));
    const truing_ramp_profile_t no_accel = { 1000000u, 4000u, 0u };
    TEST_ASSERT_FALSE(truing_ramp_plan(&r, &no_accel, 100u));
    const truing_ramp_profile_t too_fast = { 1000000u, 600000u, 8000u };   /* < 2 ticks per pulse */
    TEST_ASSERT_FALSE(truing_ramp_plan(&r, &too_fast, 100u));
    TEST_ASSERT_FALSE(truing_ramp_plan(&r, NULL, 100u));
    TEST_ASSERT_FALSE(truing_ramp_plan(NULL, &k_prof, 100u));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_ramp_step(&r));   /* an idle ramp emits nothing */
    TEST_ASSERT_TRUE(truing_ramp_duration_s(&no_accel, 100u) < 0.0f);
}

/* The count is the whole point: position is the number of pulses that actually went out. */
static void test_ramp_emits_exactly_the_planned_pulses(void)
{
    static const uint32_t k_n[] = { 1u, 2u, 3u, 4u, 7u, 10u, 999u, 1000u, 2000u, 2001u, 123457u };
    for (size_t i = 0; i < sizeof(k_n) / sizeof(k_n[0]); ++i) {
        truing_ramp_t r;
        TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, k_n[i]));
        run_ramp(&r, k_prof.tick_hz, 0u, &g_run);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(k_n[i], g_run.pulses, "pulse count");
        TEST_ASSERT_FALSE(truing_ramp_active(&r));
    }
}

static void test_ramp_reaches_but_never_exceeds_cruise(void)
{
    truing_ramp_t r;
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, 20000u));
    run_ramp(&r, k_prof.tick_hz, 0u, &g_run);
    TEST_ASSERT_EQUAL_UINT32(k_prof.tick_hz / k_prof.max_step_hz, g_run.min_delay);
}

/* Speed only rises while accelerating and only falls while decelerating (1-tick rounding). */
static void test_ramp_is_monotonic_up_and_down(void)
{
    truing_ramp_t r;
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, 3000u));
    run_ramp(&r, k_prof.tick_hz, 0u, &g_run);
    uint32_t i = 1u;
    while (i < g_run.n_delays && g_run.delays[i] <= g_run.delays[i - 1u] + 1u && g_run.delays[i] > g_run.min_delay) {
        ++i;
    }
    while (i < g_run.n_delays && g_run.delays[i] == g_run.min_delay) {
        ++i;
    }
    for (; i < g_run.n_delays; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(g_run.delays[i] + 1u >= g_run.delays[i - 1u], "deceleration never speeds up");
    }
}

/* Triangle profile: too short to reach cruise, so it turns around halfway. */
static void test_ramp_short_move_never_reaches_cruise(void)
{
    truing_ramp_t r;
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, 200u));   /* cruise needs 1000 pulses */
    run_ramp(&r, k_prof.tick_hz, 0u, &g_run);
    TEST_ASSERT_EQUAL_UINT32(200u, g_run.pulses);
    TEST_ASSERT_TRUE(g_run.min_delay > k_prof.tick_hz / k_prof.max_step_hz);
}

/* The estimate feeds navigation's move deadline, so it must track the real pulse train. */
static void test_ramp_duration_estimate_tracks_the_pulse_train(void)
{
    static const uint32_t k_n[] = { 200u, 2000u, 20000u };
    for (size_t i = 0; i < sizeof(k_n) / sizeof(k_n[0]); ++i) {
        truing_ramp_t r;
        TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, k_n[i]));
        run_ramp(&r, k_prof.tick_hz, 0u, &g_run);
        const float est = truing_ramp_duration_s(&k_prof, k_n[i]);
        TEST_ASSERT_FLOAT_WITHIN(0.10f * est, est, (float)g_run.seconds);
    }
}

static void test_ramp_starts_at_cruise_when_cruise_is_slow(void)
{
    const truing_ramp_profile_t slow = { 1000000u, 100u, 100000u };
    truing_ramp_t r;
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &slow, 50u));
    TEST_ASSERT_EQUAL_INT(TRUING_RAMP_RUN, r.phase);
    run_ramp(&r, slow.tick_hz, 0u, &g_run);
    TEST_ASSERT_EQUAL_UINT32(50u, g_run.pulses);
    TEST_ASSERT_EQUAL_UINT32(10000u, g_run.min_delay);
    TEST_ASSERT_EQUAL_UINT32(10000u, g_run.max_delay);
}

/* Controlled stop at cruise: it decelerates over about as many pulses as it accelerated. */
static void test_ramp_stop_at_cruise_decelerates(void)
{
    truing_ramp_t r;
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, 100000u));
    run_ramp(&r, k_prof.tick_hz, 5000u, &g_run);
    TEST_ASSERT_TRUE(g_run.pulses < 100000u);
    /* The recurrence reaches cruise in ~v²/2a = 1000 pulses (1004 here); a stop at cruise
     * spends exactly the pulses acceleration took. */
    TEST_ASSERT_UINT32_WITHIN(20u, 1000u, (uint32_t)r.accel_steps);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)r.accel_steps, g_run.after_stop);
    TEST_ASSERT_FALSE(truing_ramp_active(&r));
}

static void test_ramp_stop_while_accelerating(void)
{
    truing_ramp_t r;
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, 100000u));
    run_ramp(&r, k_prof.tick_hz, 100u, &g_run);
    TEST_ASSERT_TRUE(g_run.after_stop >= 99u && g_run.after_stop <= 101u);
}

/* A stop requested during deceleration, or on an idle ramp, changes nothing. */
static void test_ramp_stop_that_cannot_shorten_is_ignored(void)
{
    truing_ramp_t r;
    memset(&r, 0, sizeof(r));
    truing_ramp_request_stop(&r);   /* idle before any plan */
    TEST_ASSERT_FALSE(r.stop_req);
    TEST_ASSERT_TRUE(truing_ramp_plan(&r, &k_prof, 2000u));
    run_ramp(&r, k_prof.tick_hz, 1990u, &g_run);   /* already decelerating */
    TEST_ASSERT_EQUAL_UINT32(2000u, g_run.pulses);
    truing_ramp_request_stop(&r);   /* idle */
    TEST_ASSERT_FALSE(truing_ramp_active(&r));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_ramp_step(&r));
}

/* ---- TMC2209 framing ---------------------------------------------------------------- */
static void test_tmc2209_crc_and_read_request(void)
{
    /* 05 00 00 48 is the GCONF read request for node 0 as sent to real TMC2209s; it agrees
     * with the datasheet's reference CRC routine this file implements. */
    uint8_t req[TRUING_TMC2209_READ_REQ_LEN];
    truing_tmc2209_read_request(req, 0u, TRUING_TMC2209_GCONF);
    const uint8_t gconf[] = { 0x05, 0x00, 0x00, 0x48 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(gconf, req, 4);
    truing_tmc2209_read_request(req, 0u, TRUING_TMC2209_IOIN);
    TEST_ASSERT_EQUAL_HEX8(0x6F, req[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, truing_tmc2209_crc(NULL, 3u));
}

static void test_tmc2209_write_frame_layout(void)
{
    uint8_t w[TRUING_TMC2209_WRITE_LEN];
    truing_tmc2209_write_frame(w, 3u, TRUING_TMC2209_IHOLD_IRUN, 0x00011F10u);
    TEST_ASSERT_EQUAL_HEX8(0x05, w[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, w[1]);
    TEST_ASSERT_EQUAL_HEX8(0x90, w[2]);   /* register | write bit */
    TEST_ASSERT_EQUAL_HEX8(0x00, w[3]);
    TEST_ASSERT_EQUAL_HEX8(0x01, w[4]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, w[5]);
    TEST_ASSERT_EQUAL_HEX8(0x10, w[6]);
    TEST_ASSERT_EQUAL_HEX8(truing_tmc2209_crc(w, 7u), w[7]);
}

static void test_tmc2209_reply_is_validated(void)
{
    uint8_t rep[TRUING_TMC2209_REPLY_LEN] = { 0x05, 0xFF, 0x06, 0x21, 0x00, 0x00, 0x40, 0x00 };
    rep[7] = truing_tmc2209_crc(rep, 7u);
    uint32_t v = 0u;
    TEST_ASSERT_TRUE(truing_tmc2209_parse_reply(rep, TRUING_TMC2209_IOIN, &v));
    TEST_ASSERT_EQUAL_HEX32(0x21000040u, v);
    TEST_ASSERT_EQUAL_HEX8(TRUING_TMC2209_VERSION, (uint8_t)(v >> 24));

    TEST_ASSERT_FALSE(truing_tmc2209_parse_reply(rep, TRUING_TMC2209_GCONF, &v));   /* wrong register */
    uint8_t bad[TRUING_TMC2209_REPLY_LEN];
    memcpy(bad, rep, sizeof(bad));
    bad[7] ^= 0x01u;
    TEST_ASSERT_FALSE(truing_tmc2209_parse_reply(bad, TRUING_TMC2209_IOIN, &v));   /* CRC */
    memcpy(bad, rep, sizeof(bad));
    bad[1] = 0x00u;
    bad[7] = truing_tmc2209_crc(bad, 7u);
    TEST_ASSERT_FALSE(truing_tmc2209_parse_reply(bad, TRUING_TMC2209_IOIN, &v));   /* our own echo, not a reply */
    memcpy(bad, rep, sizeof(bad));
    bad[0] = 0x00u;
    bad[7] = truing_tmc2209_crc(bad, 7u);
    TEST_ASSERT_FALSE(truing_tmc2209_parse_reply(bad, TRUING_TMC2209_IOIN, &v));   /* sync */
}

static void test_tmc2209_microsteps_and_current_fields(void)
{
    TEST_ASSERT_EQUAL_UINT(256u, truing_tmc2209_chopconf_microsteps(0x10000053u));   /* datasheet reset value */
    uint32_t c = 0u;
    TEST_ASSERT_TRUE(truing_tmc2209_chopconf_set_microsteps(0x10000053u, 8u, &c));
    TEST_ASSERT_EQUAL_HEX32(0x15000053u, c);
    TEST_ASSERT_EQUAL_UINT(8u, truing_tmc2209_chopconf_microsteps(c));
    TEST_ASSERT_TRUE(truing_tmc2209_chopconf_set_microsteps(c, 1u, &c));
    TEST_ASSERT_EQUAL_UINT(1u, truing_tmc2209_chopconf_microsteps(c));
    TEST_ASSERT_FALSE(truing_tmc2209_chopconf_set_microsteps(c, 3u, &c));
    TEST_ASSERT_FALSE(truing_tmc2209_chopconf_set_microsteps(c, 512u, &c));
    TEST_ASSERT_EQUAL_UINT(0u, truing_tmc2209_chopconf_microsteps(0x0F000000u));   /* reserved MRES */

    /* A driver that reset comes back on its MS1/MS2 pins: mstep_reg_select clear, MRES at its power-up value. That must
     * read as NOT intact even when the pins happen to give the same count, and a MRES of the wrong size must too. */
    uint32_t c16 = 0u;
    TEST_ASSERT_TRUE(truing_tmc2209_chopconf_set_microsteps(0x10000053u, 16u, &c16));
    const uint32_t g_ok = TRUING_TMC2209_GCONF_PDN_DISABLE | TRUING_TMC2209_GCONF_MSTEP_REG_SELECT;
    TEST_ASSERT_TRUE(truing_tmc2209_microsteps_intact(g_ok, c16, 16u));
    TEST_ASSERT_FALSE(truing_tmc2209_microsteps_intact(g_ok, 0x10000053u, 16u));                  /* MRES back at 256 */
    TEST_ASSERT_FALSE(truing_tmc2209_microsteps_intact(TRUING_TMC2209_GCONF_PDN_DISABLE, c16, 16u));   /* pins govern */
    TEST_ASSERT_FALSE(truing_tmc2209_microsteps_intact(g_ok, c16, 8u));                          /* the halved scale */
    TEST_ASSERT_FALSE(truing_tmc2209_microsteps_intact(g_ok, c16, 0u));                          /* nothing wanted: no claim */

    TEST_ASSERT_EQUAL_HEX32(0x00011F10u, truing_tmc2209_ihold_irun(16u, 31u, 1u));
    TEST_ASSERT_EQUAL_UINT(17u, truing_tmc2209_cs_actual(0x80110000u));

    char buf[64];
    truing_tmc2209_describe_drv_status(0u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ok", buf);
    truing_tmc2209_describe_drv_status(TRUING_TMC2209_DRV_OT | TRUING_TMC2209_DRV_S2GA, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("OT S2GA", buf);
    TEST_ASSERT_TRUE((TRUING_TMC2209_DRV_FAULT_MASK & TRUING_TMC2209_DRV_OLA) == 0u);
}

/* The planned laser spoke sensor's seam (scaffold): the fake hands back scripted beam edges in order,
 * `blocked` follows the last edge, an unfitted sensor reports nothing, and a full queue counts drops. */
static void test_spoke_sensor_fake_reports_edges_in_order(void)
{
    truing_spoke_sensor_if_t sn;
    truing_spoke_sensor_fake_ctx_t c;
    truing_spoke_event_t e;
    truing_spoke_sensor_fake_init(&sn, &c, false);
    TEST_ASSERT_FALSE(sn.available(&sn));
    TEST_ASSERT_TRUE(truing_spoke_sensor_fake_push(&c, TRUING_SPOKE_EDGE_ENTER, 10u, 100));
    TEST_ASSERT_FALSE(sn.blocked(&sn));
    TEST_ASSERT_FALSE(sn.next_event(&sn, &e));
    truing_spoke_sensor_fake_init(&sn, &c, true);
    TEST_ASSERT_TRUE(sn.available(&sn));
    TEST_ASSERT_FALSE(sn.blocked(&sn));
    TEST_ASSERT_TRUE(truing_spoke_sensor_fake_push(&c, TRUING_SPOKE_EDGE_ENTER, 1000u, 400));
    TEST_ASSERT_TRUE(sn.blocked(&sn));
    TEST_ASSERT_TRUE(truing_spoke_sensor_fake_push(&c, TRUING_SPOKE_EDGE_LEAVE, 1800u, 430));
    TEST_ASSERT_FALSE(sn.blocked(&sn));
    TEST_ASSERT_TRUE(sn.next_event(&sn, &e));
    TEST_ASSERT_EQUAL_INT(TRUING_SPOKE_EDGE_ENTER, e.edge);
    TEST_ASSERT_EQUAL_UINT32(1000u, e.t_us);
    TEST_ASSERT_EQUAL_INT32(400, e.drive_steps);
    TEST_ASSERT_TRUE(sn.next_event(&sn, &e));
    TEST_ASSERT_EQUAL_INT(TRUING_SPOKE_EDGE_LEAVE, e.edge);
    TEST_ASSERT_FALSE(sn.next_event(&sn, &e));
    for (unsigned i = 0; i < TRUING_SPOKE_SENSOR_FAKE_QUEUE; ++i) {
        TEST_ASSERT_TRUE(truing_spoke_sensor_fake_push(&c, TRUING_SPOKE_EDGE_ENTER, i, (int32_t)i));
    }
    TEST_ASSERT_FALSE(truing_spoke_sensor_fake_push(&c, TRUING_SPOKE_EDGE_LEAVE, 99u, 99));
    TEST_ASSERT_EQUAL_UINT32(1u, c.dropped);
    TEST_ASSERT_TRUE(sn.next_event(&sn, &e));
    TEST_ASSERT_EQUAL_UINT32(0u, e.t_us);                         /* oldest first, after wrap-around too */
}

/* ---- bench laser-stop (beam_stop.h) --------------------------------------------------- */
static truing_beam_stop_t g_bs;

/* The trigger is one-shot, needs an ENTER while armed AND moving, and asks for exactly one stop. */
static void test_beam_stop_fires_once_on_a_break_while_moving(void)
{
    truing_beam_stop_init(&g_bs, false);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, true, 10u, 5, true));    /* disarmed: logged, no stop */
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 20u, 9, true));
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 100));
    TEST_ASSERT_EQUAL_UINT32(1u, g_bs.cur.trial);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, true, 30u, 100, false)); /* not moving: nothing to stop */
    TEST_ASSERT_TRUE(g_bs.armed);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 40u, 100, false));
    TEST_ASSERT_TRUE(truing_beam_stop_on_edge(&g_bs, true, 50u, 140, true));
    TEST_ASSERT_FALSE(g_bs.armed);
    TEST_ASSERT_TRUE(g_bs.cur.fired);
    TEST_ASSERT_EQUAL_INT32(140, g_bs.cur.trigger_pos);
    TEST_ASSERT_EQUAL_UINT32(50u, g_bs.cur.trigger_t_us);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 60u, 150, true));
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, true, 70u, 160, true));  /* a second break: no second stop */
    TEST_ASSERT_EQUAL_UINT32(1u, g_bs.stops_requested);
    TEST_ASSERT_EQUAL_UINT32(2u, g_bs.cur.edges_after);
    TEST_ASSERT_TRUE(g_bs.cur.left_beam);
    TEST_ASSERT_EQUAL_INT32(150, g_bs.cur.leave_pos);
    TEST_ASSERT_EQUAL_UINT32(7u, g_bs.edges);
    TEST_ASSERT_EQUAL_UINT32(4u, g_bs.enters);
    TEST_ASSERT_FALSE(truing_beam_stop_arm(&g_bs, 160));   /* still decelerating: the record is not disturbed */
}

/* An interrupt that reads the level it already had is a glitch, not an edge; the poll's late find is a resync. */
static void test_beam_stop_counts_glitches_and_resyncs(void)
{
    truing_beam_stop_init(&g_bs, false);
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 0));
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 5u, 0, true));
    TEST_ASSERT_EQUAL_UINT32(1u, g_bs.glitches);
    TEST_ASSERT_EQUAL_UINT32(0u, g_bs.edges);
    TEST_ASSERT_FALSE(truing_beam_stop_resync(&g_bs, false, 6u, 0, true));   /* no mismatch: nothing to resync */
    TEST_ASSERT_TRUE(truing_beam_stop_resync(&g_bs, true, 7u, 12, true));    /* a late break still stops */
    TEST_ASSERT_EQUAL_UINT32(1u, g_bs.resyncs);
    TEST_ASSERT_EQUAL_INT32(12, g_bs.cur.trigger_pos);
}

/* Armed with the beam already blocked: only the NEXT break triggers, after the beam has cleared. */
static void test_beam_stop_armed_while_blocked_waits_for_the_next_break(void)
{
    truing_beam_stop_init(&g_bs, true);
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 0));
    TEST_ASSERT_TRUE(g_bs.cur.blocked_at_arm);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 10u, 30, true));
    TEST_ASSERT_TRUE(g_bs.armed);
    TEST_ASSERT_TRUE(truing_beam_stop_on_edge(&g_bs, true, 20u, 400, true));
}

/* The poll closes a trial only once the pulse train is over, and classifies it by the beam at that moment. */
static void test_beam_stop_poll_classifies_the_end(void)
{
    truing_beam_stop_init(&g_bs, false);
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 0));
    TEST_ASSERT_FALSE(truing_beam_stop_poll(&g_bs, false, 0, 1u, true, false, 800u, 1600u));   /* not moved yet */
    TEST_ASSERT_TRUE(g_bs.armed);
    TEST_ASSERT_TRUE(truing_beam_stop_on_edge(&g_bs, true, 1000u, 500, true));
    TEST_ASSERT_FALSE(truing_beam_stop_poll(&g_bs, true, 560, 1500u, true, false, 800u, 1600u));
    TEST_ASSERT_EQUAL_UINT32(800u, g_bs.cur.max_step_hz);
    TEST_ASSERT_TRUE(truing_beam_stop_poll(&g_bs, false, 700, 251000u, true, false, 9999u, 9999u));
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_STOPPED_IN_BEAM, g_bs.last.result);
    TEST_ASSERT_EQUAL_INT32(700, g_bs.last.final_pos);
    TEST_ASSERT_EQUAL_UINT32(250000u, g_bs.last.end_t_us - g_bs.last.trigger_t_us);
    TEST_ASSERT_EQUAL_UINT32(800u, g_bs.last.max_step_hz);   /* the ramp the move ran at, not a later one */
    TEST_ASSERT_FALSE(truing_beam_stop_poll(&g_bs, false, 700, 260000u, true, false, 800u, 1600u));   /* closed */

    /* the spoke left before the last pulse */
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 700));
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 300000u, 720, true));
    TEST_ASSERT_TRUE(truing_beam_stop_on_edge(&g_bs, true, 310000u, 900, true));
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 320000u, 960, true));
    TEST_ASSERT_TRUE(truing_beam_stop_poll(&g_bs, false, 1100, 400000u, true, false, 800u, 1600u));
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_PASSED_THROUGH, g_bs.last.result);
    TEST_ASSERT_EQUAL_INT32(960, g_bs.last.leave_pos);
    TEST_ASSERT_EQUAL_UINT32(2u, g_bs.last.trial);

    /* cleared after the break and blocked again by the last pulse: not "the spoke that broke it" */
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 1100));
    TEST_ASSERT_TRUE(truing_beam_stop_on_edge(&g_bs, true, 400000u, 1200, true));
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 401000u, 1260, true));
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, true, 410000u, 1600, true));
    TEST_ASSERT_TRUE(truing_beam_stop_poll(&g_bs, false, 1620, 420000u, true, false, 800u, 1600u));
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_BLOCKED_AGAIN, g_bs.last.result);
    TEST_ASSERT_EQUAL_INT32(1260, g_bs.last.leave_pos);
    TEST_ASSERT_EQUAL_UINT32(2u, g_bs.last.edges_after);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, false, 430000u, 1620, false));   /* clear again for the next */

    /* coils released mid-deceleration: the count ended, but not by the controlled stop */
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 1100));
    TEST_ASSERT_TRUE(truing_beam_stop_on_edge(&g_bs, true, 500000u, 1300, true));
    TEST_ASSERT_TRUE(truing_beam_stop_poll(&g_bs, false, 1310, 500100u, false, false, 800u, 1600u));
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_INTERRUPTED, g_bs.last.result);
}

/* Armed through a whole move with no break: NO_TRIGGER, recorded once, then disarmed. Disarming an unfired
 * trial drops it without a record. */
static void test_beam_stop_move_without_a_break(void)
{
    truing_beam_stop_init(&g_bs, false);
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 0));
    TEST_ASSERT_FALSE(truing_beam_stop_poll(&g_bs, true, 300, 1u, true, false, 500u, 1000u));
    TEST_ASSERT_TRUE(truing_beam_stop_poll(&g_bs, false, 5000, 2u, true, false, 500u, 1000u));
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_NO_TRIGGER, g_bs.last.result);
    TEST_ASSERT_FALSE(g_bs.armed);
    TEST_ASSERT_EQUAL_UINT32(0u, g_bs.stops_requested);

    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 5000));
    truing_beam_stop_disarm(&g_bs);
    TEST_ASSERT_FALSE(truing_beam_stop_on_edge(&g_bs, true, 3u, 5100, true));
    TEST_ASSERT_FALSE(truing_beam_stop_poll(&g_bs, false, 5200, 4u, true, false, 500u, 1000u));
    TEST_ASSERT_EQUAL_UINT32(1u, g_bs.last.trial);   /* the disarmed trial left no record */
}

/* The edge log hands back edges newer than a sequence number, oldest first, and says how many it lost. */
static void test_beam_stop_edge_log(void)
{
    truing_beam_edge_rec_t out[TRUING_BEAM_LOG_LEN];
    uint32_t lost = 99u;
    truing_beam_stop_init(&g_bs, false);
    TEST_ASSERT_EQUAL_UINT(0u, truing_beam_stop_edges_since(&g_bs, 0u, out, TRUING_BEAM_LOG_LEN, &lost));
    TEST_ASSERT_EQUAL_UINT32(0u, lost);
    for (int32_t i = 0; i < 40; ++i) {
        (void)truing_beam_stop_on_edge(&g_bs, (i % 2) == 0, (uint32_t)i, i * 10, true);
    }
    TEST_ASSERT_EQUAL_UINT32(40u, g_bs.log_seq);
    size_t n = truing_beam_stop_edges_since(&g_bs, 35u, out, TRUING_BEAM_LOG_LEN, &lost);
    TEST_ASSERT_EQUAL_UINT(5u, n);
    TEST_ASSERT_EQUAL_UINT32(36u, out[0].seq);
    TEST_ASSERT_EQUAL_INT32(350, out[0].pos);
    TEST_ASSERT_EQUAL_INT(TRUING_SPOKE_EDGE_LEAVE, out[0].edge);   /* edge 36 is i = 35: odd, the beam cleared */
    n = truing_beam_stop_edges_since(&g_bs, 0u, out, TRUING_BEAM_LOG_LEN, &lost);
    TEST_ASSERT_EQUAL_UINT(TRUING_BEAM_LOG_LEN, n);
    TEST_ASSERT_EQUAL_UINT32(8u, lost);
    TEST_ASSERT_EQUAL_UINT32(9u, out[0].seq);
}

/* The trigger driving the real ramp: a break at cruise asks for the ramp's own controlled stop, and the
 * pulses after it are exactly what the ramp spends decelerating (about v^2/2a). Whether the spoke is still
 * in the beam at the end depends only on how wide the blocked stretch is against that distance -- the
 * relationship the bench trials measure. Test numbers, not a motor's. */
static void run_beam_trial(int32_t blocked_width, truing_ramp_t *r)
{
    truing_beam_stop_init(&g_bs, false);
    TEST_ASSERT_TRUE(truing_beam_stop_arm(&g_bs, 0));
    TEST_ASSERT_TRUE(truing_ramp_plan(r, &k_prof, 50000u));
    int32_t pos = 0, enter_at = 20000;
    bool requested = false;
    for (;;) {
        pos++;   /* one pulse out, as the step ISR counts it */
        if (pos == enter_at) {
            requested = truing_beam_stop_on_edge(&g_bs, true, (uint32_t)pos, pos, true);
            if (requested) {
                truing_ramp_request_stop(r);
            }
        }
        if (pos == enter_at + blocked_width) {
            (void)truing_beam_stop_on_edge(&g_bs, false, (uint32_t)pos, pos, true);
        }
        if (truing_ramp_step(r) == 0u) {
            break;
        }
    }
    TEST_ASSERT_TRUE(requested);
    TEST_ASSERT_TRUE(truing_beam_stop_poll(&g_bs, false, pos, (uint32_t)pos + 1u, true, false, k_prof.max_step_hz,
                                           k_prof.accel_steps_s2));
}

static void test_beam_stop_overshoot_is_the_ramps_own_deceleration(void)
{
    truing_ramp_t r;
    run_beam_trial(3000, &r);
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_STOPPED_IN_BEAM, g_bs.last.result);
    TEST_ASSERT_EQUAL_INT32(r.accel_steps, g_bs.last.final_pos - g_bs.last.trigger_pos);
    TEST_ASSERT_INT32_WITHIN(20, 1000, g_bs.last.final_pos - g_bs.last.trigger_pos);   /* 4000^2 / (2 * 8000) */

    run_beam_trial(400, &r);   /* a blocked stretch narrower than the stopping distance */
    TEST_ASSERT_EQUAL_INT(TRUING_BEAM_RESULT_PASSED_THROUGH, g_bs.last.result);
    TEST_ASSERT_EQUAL_INT32(20400, g_bs.last.leave_pos);
}

/* ---- laser-guided alignment controller (beam_align.h) ------------------------------------ */
/* A simulated bench standing in for src/laser_bench.c: a ramp-timed step generator (the same truing_ramp the
 * TMC2209 driver runs), a wheel whose spokes block the beam over [k*pitch + phase, + width) in wheel steps, an
 * optional backlash deadband between drive and wheel, a 2 ms tick, and the glue's command execution. The
 * geometry numbers are TEST values, not a wheel's. */
typedef struct {
    truing_beam_align_t a;
    truing_ramp_t ramp;
    truing_ramp_profile_t prof;
    bool moving, coils, faulted;
    int32_t pos, dir;
    double wheel;            /* wheel position in drive steps, after backlash */
    int32_t backlash;        /* total deadband, steps */
    int32_t pitch, width, phase;
    uint64_t t_us, next_pulse_us, next_tick_us;
    bool beam;
    bool refuse_moves;
    uint32_t stop_requests;
} align_sim_t;

static align_sim_t g_sim;

static bool sim_beam(const align_sim_t *s)
{
    int32_t w = (int32_t)(s->wheel >= 0 ? s->wheel + 0.5 : s->wheel - 0.5) - s->phase;
    int32_t m = ((w % s->pitch) + s->pitch) % s->pitch;
    return m < s->width;
}

static void sim_init(align_sim_t *s, int32_t pitch, int32_t width, int32_t phase, uint32_t hz, uint32_t accel)
{
    memset(s, 0, sizeof(*s));
    s->pitch = pitch;
    s->width = width;
    s->phase = phase;
    s->prof = (truing_ramp_profile_t){ 1000000u, hz, accel };
    s->coils = true;
    s->next_tick_us = 2000u;
    s->beam = sim_beam(s);
    truing_beam_align_init(&s->a, s->beam);
}

static bool sim_move(align_sim_t *s, int32_t steps)
{
    if (s->moving || s->refuse_moves || steps == 0 || !truing_ramp_plan(&s->ramp, &s->prof, (uint32_t)abs(steps))) {
        return false;
    }
    s->dir = steps > 0 ? 1 : -1;
    s->moving = true;
    s->next_pulse_us = s->t_us + (uint64_t)s->ramp.delay;
    return true;
}

static void sim_exec(align_sim_t *s, const truing_align_cmd_t *c)
{
    if (c->set_motion && !s->moving) {
        s->prof.max_step_hz = c->hz;
        s->prof.accel_steps_s2 = c->accel;
    }
    if (c->move && !sim_move(s, c->steps)) {
        truing_beam_align_command_failed(&s->a, (uint32_t)s->t_us, s->pos);
    }
}

static void sim_pulse(align_sim_t *s)
{
    s->pos += s->dir;
    const double half = s->backlash / 2.0;
    if (s->pos - s->wheel > half) {
        s->wheel = s->pos - half;
    } else if (s->wheel - s->pos > half) {
        s->wheel = s->pos + half;
    }
    const bool b = sim_beam(s);
    const uint32_t next = truing_ramp_step(&s->ramp);
    s->moving = next != 0u;
    s->next_pulse_us += next;
    if (b != s->beam) {
        s->beam = b;
        /* The interrupt sees the drive still moving when the edge comes from a pulse it just sent. */
        if (truing_beam_align_on_edge(&s->a, b, (uint32_t)s->t_us, s->pos, true)) {
            s->stop_requests++;
            truing_ramp_request_stop(&s->ramp);
        }
    }
}

/* Run until the controller finishes (or `max_us` of board time passes). */
static bool sim_run(align_sim_t *s, uint64_t max_us)
{
    const uint64_t end = s->t_us + max_us;
    while (s->t_us < end) {
        if (s->moving && s->next_pulse_us <= s->next_tick_us) {
            s->t_us = s->next_pulse_us;
            sim_pulse(s);
            continue;
        }
        s->t_us = s->next_tick_us;
        s->next_tick_us += 2000u;
        truing_align_cmd_t c;
        const bool done = truing_beam_align_tick(&s->a, (uint32_t)s->t_us, s->pos, s->moving, s->coils, s->faulted, &c);
        sim_exec(s, &c);
        if (done) {
            /* one more tick lets the controller restore the operator's ramp */
            truing_beam_align_tick(&s->a, (uint32_t)s->t_us + 2000u, s->pos, s->moving, s->coils, s->faulted, &c);
            sim_exec(s, &c);
            return true;
        }
    }
    return false;
}

static truing_align_config_t align_cfg(truing_align_mode_t mode, truing_align_strategy_t st, uint16_t target)
{
    truing_align_config_t c;
    memset(&c, 0, sizeof(c));
    c.mode = mode;
    c.strategy = st;
    c.dir = 1;
    c.target_break = target;
    c.max_steps = 20000u;
    c.settle_ms = 100u;
    c.slow_hz = 200u;
    c.slow_accel = 1000u;
    c.reacq_max_steps = 5000u;
    return c;
}

static bool sim_start(align_sim_t *s, const truing_align_config_t *c)
{
    truing_align_cmd_t cmd;
    const char *why = NULL;
    if (!truing_beam_align_start(&s->a, c, s->pos, s->moving, s->beam, (uint32_t)s->t_us, s->prof.max_step_hz,
                                 s->prof.accel_steps_s2, &cmd, &why)) {
        return false;
    }
    sim_exec(s, &cmd);
    return true;
}

/* Stage 0: stop on the 33rd break from a clear start = 32 passages after spoke 0, every edge kept. */
static void test_align_count_run_stops_on_the_nth_break(void)
{
    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 400u, 2000u);   /* first spoke 200 steps ahead */
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_COUNT, TRUING_ALIGN_STRAT_STOP, 33u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_COUNT_DONE, s->a.result);
    TEST_ASSERT_EQUAL_UINT16(33u, s->a.breaks);
    TEST_ASSERT_EQUAL_INT32(200 + 32 * 400, s->a.trigger_pos);
    TEST_ASSERT_EQUAL_UINT32(1u, s->stop_requests);
    TEST_ASSERT_EQUAL_INT32(400, s->a.pitch_med);
    TEST_ASSERT_EQUAL_INT32(60, s->a.width_med);
    TEST_ASSERT_TRUE(s->a.n_edges >= 65u);
    TEST_ASSERT_TRUE(s->a.stop_pos - s->a.trigger_pos <= 41);   /* ~v^2/2a = 40 from cruise */
}

/* A: a slow approach stops inside the beam; a fast one carries the wheel spokes past it. */
static void test_align_stop_strategy_slow_aligns_fast_overshoots(void)
{
    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 200u, 1000u);   /* stop distance ~20 < width 60 */
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_STOP, 2u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_EQUAL_INT32(600, s->a.trigger_pos);   /* the second spoke, not the first */
    TEST_ASSERT_EQUAL_INT8(0, s->a.final_zone);

    /* Fast: the trigger (600 steps in) comes while still accelerating, so the stop spends as many pulses as the
     * acceleration did -- about 600, carrying the wheel past the target and into the next spoke or beyond. */
    sim_init(s, 400, 60, 200, 2000u, 1000u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_OVERSHOOT, s->a.result);
    TEST_ASSERT_INT32_WITHIN(5, 600, s->a.stop_pos - s->a.trigger_pos);
    TEST_ASSERT_TRUE(s->a.final_zone >= 2);
    TEST_ASSERT_TRUE(s->a.target_left);
    TEST_ASSERT_EQUAL_INT32(s->a.trigger_pos + 60, s->a.target_leave_pos);
}

/* C: after an overshoot of several spokes the slow reverse undoes the edges one by one and stops on the same
 * spoke; with `center` it ends in the middle of the measured blocked span. With backlash the re-found edge
 * moves by the deadband -- that offset is recorded, and still passes the same-spoke check. */
static void test_align_reacquire_finds_the_same_spoke(void)
{
    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 2000u, 1000u);
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REACQUIRE, 3u);
    c.center = true;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_TRUE(s->a.reacquired);
    TEST_ASSERT_EQUAL_UINT16(1u, s->a.reacquisitions);
    TEST_ASSERT_TRUE(s->a.same_spoke_checked && s->a.same_spoke_ok);
    TEST_ASSERT_INT32_WITHIN(2, s->a.target_leave_pos, s->a.reacq_pos);
    TEST_ASSERT_INT32_WITHIN(3, s->a.trigger_pos + 30, s->a.final_pos);   /* the middle of 60 */
    TEST_ASSERT_EQUAL_UINT32(2u, s->stop_requests);                      /* the trigger, then the re-find */
    TEST_ASSERT_EQUAL_UINT32(2000u, s->prof.max_step_hz);                /* the operator's ramp is back */

    sim_init(s, 400, 60, 200, 2000u, 1000u);
    s->backlash = 12;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_INT32_WITHIN(2, s->a.target_leave_pos - 12, s->a.reacq_pos);   /* the deadband, measured */
}

/* D: REACQUIRE_CLEAR re-finds the spoke like C but does not stop there: it keeps reversing through the spoke and
 * takes the controlled stop when the beam reads clear on the far side. The finish is "just cleared the spoke":
 * zone -1, beam clear, one slow-ramp stopping distance past the spoke's near edge. It never centres. */
/* The same-spoke test judges against the SHORTEST gap, not the median: with spacing 1963/718/1963/3255 the median of a
 * two-gap approach is 1963, and a neighbour's edge 400 steps off would have passed as "the same spoke". */
static void test_align_same_spoke_uses_the_shortest_gap(void)
{
    TEST_ASSERT_TRUE(truing_beam_align_same_spoke(12, 718));
    TEST_ASSERT_TRUE(truing_beam_align_same_spoke(-300, 718));
    TEST_ASSERT_FALSE(truing_beam_align_same_spoke(400, 718));     /* half of 718 is 359 */
    TEST_ASSERT_FALSE(truing_beam_align_same_spoke(-400, 718));
    TEST_ASSERT_TRUE(truing_beam_align_same_spoke(400, 1963));     /* what the median let through */
    TEST_ASSERT_TRUE(truing_beam_align_same_spoke(5000, 0));       /* no gap measured: no verdict */

    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 2000u, 1000u);
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REACQUIRE, 3u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT32(400, s->a.pitch_min);
    TEST_ASSERT_TRUE(s->a.same_spoke_checked && s->a.same_spoke_ok);
}

static void test_align_reacquire_clear_ends_just_past_the_spoke(void)
{
    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 800u, 1000u);   /* stop distance ~320: past the target, in the gap after it */
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REACQUIRE_CLEAR, 1u);
    c.center = true;                           /* ignored by this strategy */
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_TRUE(s->a.reacquired);
    TEST_ASSERT_TRUE(s->a.cleared);
    TEST_ASSERT_EQUAL_INT8(-1, s->a.final_zone);
    TEST_ASSERT_FALSE(s->a.final_blocked);
    TEST_ASSERT_EQUAL_UINT32(2u, s->stop_requests);                      /* the trigger, then the far-side clear */
    TEST_ASSERT_INT32_WITHIN(2, s->a.trigger_pos, s->a.clear_pos);       /* the spoke's near edge, going back */
    TEST_ASSERT_TRUE(s->a.final_pos < s->a.trigger_pos);                 /* wholly on the near side of the spoke */
    TEST_ASSERT_INT32_WITHIN(6, s->a.trigger_pos - 20, s->a.final_pos);  /* 200^2 / (2 * 1000) at the slow ramp */
    TEST_ASSERT_EQUAL_UINT32(800u, s->prof.max_step_hz);                 /* the operator's ramp is back */

    /* "a bit more after unblocked": offset_steps -50 moves 50 steps further back, still clear, still zone -1 */
    const int32_t clear_stop = s->a.final_pos;
    sim_init(s, 400, 60, 200, 800u, 1000u);
    c.reacq_max_steps = 5000u;
    c.offset_steps = -50;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_EQUAL_INT8(-1, s->a.final_zone);
    TEST_ASSERT_FALSE(s->a.final_blocked);
    TEST_ASSERT_EQUAL_INT32(-50, s->a.trim_cmd);
    TEST_ASSERT_INT32_WITHIN(2, clear_stop - 50, s->a.final_pos);
    c.offset_steps = 0;

    /* the reverse search ends inside the spoke (30 steps past its near edge): re-found, never cleared ->
     * REACQUISITION_FAILED. The first stop's distance sets how far back that is. */
    const int32_t overshoot = s->a.stop_pos - s->a.trigger_pos;
    sim_init(s, 400, 60, 200, 800u, 1000u);
    c.reacq_max_steps = (uint32_t)(overshoot - 30);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 120000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_REACQUISITION_FAILED, s->a.result);
    TEST_ASSERT_TRUE(s->a.reacquired);
    TEST_ASSERT_FALSE(s->a.cleared);

    truing_align_strategy_t st;
    TEST_ASSERT_TRUE(truing_align_strategy_parse("REACQUIRE_CLEAR", &st));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_STRAT_REACQUIRE_CLEAR, st);
    TEST_ASSERT_EQUAL_STRING("REACQUIRE_CLEAR", truing_align_strategy_str(st));
}

/* B': a step-based return to the trigger edge works while the counter tracks the wheel... */
static void test_align_return_to_edge_and_fixed_reverse(void)
{
    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 800u, 1000u);   /* stop distance ~320: in the gap after the target */
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_RETURN_TO_EDGE, 1u);
    c.center = true;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT8(1, s->a.zone_after_stop);
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_EQUAL_INT32(s->a.trigger_pos + 30, s->a.final_pos);

    /* ...and a fixed reverse of the wrong size is classified as such, with the zone it ended in. */
    sim_init(s, 400, 60, 200, 800u, 1000u);
    c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REVERSE_FIXED, 1u);
    c.reverse_steps = 50u;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_REVERSE_FAILED, s->a.result);
    TEST_ASSERT_EQUAL_INT8(1, s->a.final_zone);
}

static void test_align_failure_classes(void)
{
    align_sim_t *s = &g_sim;
    /* no spoke within the travel bound */
    sim_init(s, 4000, 60, 3000, 400u, 2000u);
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_STOP, 1u);
    c.max_steps = 1000u;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_LASER_MISSED, s->a.result);

    /* the reverse search runs out */
    sim_init(s, 400, 60, 200, 2000u, 1000u);
    c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REACQUIRE, 1u);
    c.reacq_max_steps = 100u;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_REACQUISITION_FAILED, s->a.result);

    /* a manual stop before the target: ABORTED, not "missed" */
    sim_init(s, 400, 60, 200, 400u, 2000u);
    c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_STOP, 5u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_FALSE(sim_run(s, 500000u));
    truing_ramp_request_stop(&s->ramp);   /* `roll stop` */
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ABORTED, s->a.result);
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_DETAIL_STOPPED_EARLY, s->a.detail);

    /* coils released mid-run */
    sim_init(s, 400, 60, 200, 400u, 2000u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_FALSE(sim_run(s, 500000u));
    s->coils = false;
    s->moving = false;
    TEST_ASSERT_TRUE(sim_run(s, 10000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_FAULT, s->a.result);

    /* operator abort, then the operator's ramp comes back once still */
    sim_init(s, 400, 60, 200, 2000u, 1000u);
    c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REACQUIRE, 4u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    for (int i = 0; i < 20000 && s->a.phase != TRUING_ALIGN_PHASE_CORRECT; ++i) {
        sim_run(s, 2000u);                                    /* into the slow reverse search */
    }
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_PHASE_CORRECT, s->a.phase);
    TEST_ASSERT_FALSE(sim_run(s, 100000u));
    TEST_ASSERT_EQUAL_UINT32(200u, s->prof.max_step_hz);
    TEST_ASSERT_TRUE(truing_beam_align_abort(&s->a, (uint32_t)s->t_us, s->pos));
    truing_ramp_request_stop(&s->ramp);
    TEST_ASSERT_FALSE(truing_beam_align_active(&s->a));
    sim_run(s, 2000000u);
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ABORTED, s->a.result);
    TEST_ASSERT_EQUAL_UINT32(2000u, s->prof.max_step_hz);

    /* motion nobody here commanded during the settle */
    sim_init(s, 400, 60, 200, 200u, 1000u);
    c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_STOP, 1u);
    c.settle_ms = 2000u;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    for (int i = 0; i < 5000 && s->a.phase != TRUING_ALIGN_PHASE_SETTLE; ++i) {
        sim_run(s, 2000u);
    }
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_PHASE_SETTLE, s->a.phase);
    TEST_ASSERT_TRUE(sim_move(s, 500));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ABORTED, s->a.result);
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_DETAIL_UNEXPECTED_MOTION, s->a.detail);

    /* a refused move */
    sim_init(s, 400, 60, 200, 400u, 2000u);
    s->refuse_moves = true;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_FAULT, s->a.result);
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_DETAIL_DRIVE_REFUSED, s->a.detail);
}

/* Edges while the drive is still have no direction: an even number (out and back) is tolerated, an odd number
 * leaves the beam contradicting the zone, which is AMBIGUOUS rather than a guess. */
static void test_align_settle_chatter_parity(void)
{
    align_sim_t *s = &g_sim;
    sim_init(s, 400, 60, 200, 200u, 1000u);
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_STOP, 1u);
    c.settle_ms = 50u;
    TEST_ASSERT_TRUE(sim_start(s, &c));
    for (int i = 0; i < 5000 && s->a.phase != TRUING_ALIGN_PHASE_SETTLE; ++i) {
        sim_run(s, 2000u);
    }
    TEST_ASSERT_FALSE(truing_beam_align_on_edge(&s->a, false, (uint32_t)s->t_us, s->pos, false));
    TEST_ASSERT_FALSE(truing_beam_align_on_edge(&s->a, true, (uint32_t)s->t_us, s->pos, false));
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_ALIGNED, s->a.result);
    TEST_ASSERT_EQUAL_UINT16(2u, s->a.edges_still);

    sim_init(s, 400, 60, 200, 200u, 1000u);
    TEST_ASSERT_TRUE(sim_start(s, &c));
    for (int i = 0; i < 5000 && s->a.phase != TRUING_ALIGN_PHASE_SETTLE; ++i) {
        sim_run(s, 2000u);
    }
    TEST_ASSERT_FALSE(truing_beam_align_on_edge(&s->a, false, (uint32_t)s->t_us, s->pos, false));
    s->beam = false;
    TEST_ASSERT_TRUE(sim_run(s, 60000000u));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_RESULT_LASER_AMBIGUOUS, s->a.result);
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_DETAIL_PARITY, s->a.detail);
}

static void test_align_start_refusals_and_names(void)
{
    truing_beam_align_t a;
    truing_align_cmd_t cmd;
    const char *why = NULL;
    truing_beam_align_init(&a, false);
    truing_align_config_t c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_STOP, 1u);
    TEST_ASSERT_FALSE(truing_beam_align_start(&a, &c, 0, true, false, 0u, 400u, 2000u, &cmd, &why));   /* moving */
    c.dir = 0;
    TEST_ASSERT_FALSE(truing_beam_align_start(&a, &c, 0, false, false, 0u, 400u, 2000u, &cmd, &why));
    c = align_cfg(TRUING_ALIGN_MODE_ALIGN, TRUING_ALIGN_STRAT_REVERSE_FIXED, 1u);
    TEST_ASSERT_FALSE(truing_beam_align_start(&a, &c, 0, false, false, 0u, 400u, 2000u, &cmd, &why));  /* no steps */
    c.reverse_steps = 10u;
    c.slow_hz = 0u;
    TEST_ASSERT_FALSE(truing_beam_align_start(&a, &c, 0, false, false, 0u, 400u, 2000u, &cmd, &why));
    c.slow_hz = 200u;
    TEST_ASSERT_TRUE(truing_beam_align_start(&a, &c, 0, false, false, 0u, 400u, 2000u, &cmd, &why));
    TEST_ASSERT_TRUE(cmd.move);
    TEST_ASSERT_EQUAL_INT32(20000, cmd.steps);
    TEST_ASSERT_FALSE(cmd.set_motion);   /* the approach runs at the operator's ramp */
    TEST_ASSERT_FALSE(truing_beam_align_start(&a, &c, 0, false, false, 0u, 400u, 2000u, &cmd, &why));  /* busy */
    truing_align_strategy_t st;
    TEST_ASSERT_TRUE(truing_align_strategy_parse("REACQUIRE", &st));
    TEST_ASSERT_EQUAL_INT(TRUING_ALIGN_STRAT_REACQUIRE, st);
    TEST_ASSERT_FALSE(truing_align_strategy_parse("FAST", &st));
    TEST_ASSERT_EQUAL_STRING("REACQUISITION_FAILED", truing_align_result_str(TRUING_ALIGN_RESULT_REACQUISITION_FAILED));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_ramp_refuses_what_it_cannot_time);
    RUN_TEST(test_ramp_emits_exactly_the_planned_pulses);
    RUN_TEST(test_ramp_reaches_but_never_exceeds_cruise);
    RUN_TEST(test_ramp_is_monotonic_up_and_down);
    RUN_TEST(test_ramp_short_move_never_reaches_cruise);
    RUN_TEST(test_ramp_duration_estimate_tracks_the_pulse_train);
    RUN_TEST(test_ramp_starts_at_cruise_when_cruise_is_slow);
    RUN_TEST(test_ramp_stop_at_cruise_decelerates);
    RUN_TEST(test_ramp_stop_while_accelerating);
    RUN_TEST(test_ramp_stop_that_cannot_shorten_is_ignored);
    RUN_TEST(test_tmc2209_crc_and_read_request);
    RUN_TEST(test_tmc2209_write_frame_layout);
    RUN_TEST(test_tmc2209_reply_is_validated);
    RUN_TEST(test_tmc2209_microsteps_and_current_fields);
    RUN_TEST(test_spoke_sensor_fake_reports_edges_in_order);
    RUN_TEST(test_beam_stop_fires_once_on_a_break_while_moving);
    RUN_TEST(test_beam_stop_counts_glitches_and_resyncs);
    RUN_TEST(test_beam_stop_armed_while_blocked_waits_for_the_next_break);
    RUN_TEST(test_beam_stop_poll_classifies_the_end);
    RUN_TEST(test_beam_stop_move_without_a_break);
    RUN_TEST(test_beam_stop_edge_log);
    RUN_TEST(test_beam_stop_overshoot_is_the_ramps_own_deceleration);
    RUN_TEST(test_align_count_run_stops_on_the_nth_break);
    RUN_TEST(test_align_stop_strategy_slow_aligns_fast_overshoots);
    RUN_TEST(test_align_reacquire_finds_the_same_spoke);
    RUN_TEST(test_align_same_spoke_uses_the_shortest_gap);
    RUN_TEST(test_align_reacquire_clear_ends_just_past_the_spoke);
    RUN_TEST(test_align_return_to_edge_and_fixed_reverse);
    RUN_TEST(test_align_failure_classes);
    RUN_TEST(test_align_settle_chatter_parity);
    RUN_TEST(test_align_start_refusals_and_names);
    return UNITY_END();
}
