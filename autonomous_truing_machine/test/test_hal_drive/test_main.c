/* Wheel-drive building blocks that need no hardware: the trapezoidal step ramp that times the
 * STEP pulses, and TMC2209 UART framing (SPEC §10A.6, §10A.8). The target only adds a timer
 * that emits one pulse per ramp step and a UART that moves the bytes. */
#include <string.h>
#include <unity.h>

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
    return UNITY_END();
}
