/* The SPEC §12 endpoint end to end: telemetry drained as frames, commands decoded and
 * ruled on by the orchestrator, and the two required queries answered — all over the
 * wire, with no hardware and no transport. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/auto_operator.h"
#include "truing_orch/orchestrator.h"
#include "truing_proto/json.h"
#include "truing_proto/session.h"

/* ---- a recording sink ---------------------------------------------------------------- */
#define CAP_SLOTS 24u
#define CAP_BYTES 2048u

typedef struct {
    char     frame[CAP_SLOTS][CAP_BYTES];
    size_t   len[CAP_SLOTS];
    uint32_t count;        /* total offered, including any refused */
    uint32_t refused;
    bool     refuse;       /* make send() fail, to exercise the drop path */
} cap_t;

static bool cap_send(truing_wire_sink_t *self, const char *frame, size_t len)
{
    cap_t *c = (cap_t *)self->ctx;
    if (c->refuse) {
        c->refused++;
        return false;
    }
    const uint32_t slot = c->count % CAP_SLOTS;
    const size_t n = len < CAP_BYTES - 1u ? len : CAP_BYTES - 1u;
    memcpy(c->frame[slot], frame, n);
    c->frame[slot][n] = '\0';
    c->len[slot] = len;
    c->count++;
    return true;
}

/* ---- the rig ------------------------------------------------------------------------- */
typedef struct {
    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_chain_profile_t chain;
    truing_tension_model_profile_t tmodel;
    truing_machine_profile_t machine;
    truing_fake_clock_t fc;
    truing_clock_if_t clock;
    truing_acoustic_if_t acoustic;
    truing_acoustic_synthetic_ctx_t actx;
    truing_runout_if_t runout;
    truing_runout_manual_ctx_t rctx;
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_calc_if_t calc;
    truing_calc_synthetic_ctx_t cctx;
    truing_telemetry_if_t telemetry;
    truing_telemetry_ring_ctx_t ring;
    truing_auto_operator_t op;
    truing_orchestrator_t orch;
    truing_wire_sink_t sink;
    cap_t cap;
    truing_wire_session_t session;
} rig_t;

static rig_t g;   /* large: static */

static void rig_build(void)
{
    memset(&g, 0, sizeof(g));
    truing_fixture_wheel_class_sym32(&g.wheel);
    truing_fixture_solver_config(&g.solver, 32u);
    truing_fixture_chain_profile_inmp441(&g.chain);
    truing_fixture_tension_model_profile_complete(&g.tmodel);
    truing_fixture_machine_profile(&g.machine);
    truing_fake_clock_init(&g.fc, &g.clock, 1000u);
    truing_acoustic_synthetic_init(&g.acoustic, &g.actx, g.clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    g.actx.snr_db = 30.0f;
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_acoustic_synthetic_set_spoke(&g.actx, i, 1000.0f, 480.0f);
    }
    truing_runout_manual_init(&g.runout, &g.rctx, g.clock);
    truing_navigation_manual_init(&g.nav, &g.nctx, g.clock, 32u, 32u, &g.machine);
    truing_calc_synthetic_init(&g.calc, &g.cctx, 42u);
    /* The orchestrator's telemetry sink is the ring; the session drains it (SPEC §12.2). */
    truing_telemetry_ring_init(&g.telemetry, &g.ring);
    truing_auto_operator_init(&g.op);
    float zeros[TRUING_MAX_RIM_ANGLES];
    memset(zeros, 0, sizeof(zeros));
    truing_auto_operator_set_wheel(&g.op, 32u, zeros, zeros, 1.0f);

    truing_orch_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.wheel = &g.wheel;
    deps.solver = &g.solver;
    deps.chain = &g.chain;
    deps.tension_model = &g.tmodel;
    deps.machine = &g.machine;
    deps.acoustic = &g.acoustic;
    deps.runout = &g.runout;
    deps.navigation = &g.nav;
    deps.calc = &g.calc;
    deps.telemetry = &g.telemetry;
    deps.clock = g.clock;
    deps.firmware_version = "test";
    TEST_ASSERT_TRUE(truing_orch_init(&g.orch, &deps));

    g.sink.impl_name = "capture";
    g.sink.send = cap_send;
    g.sink.ctx = &g.cap;
    TEST_ASSERT_TRUE(truing_wire_session_init(&g.session, &g.orch, &g.ring, &g.sink, false));
}

void setUp(void)
{
    rig_build();
}
void tearDown(void) {}

static const char *last_frame(void)
{
    TEST_ASSERT_TRUE(g.cap.count > 0u);
    return g.cap.frame[(g.cap.count - 1u) % CAP_SLOTS];
}

/* The most recent frame whose "t" is `kind`, or NULL. */
static const char *last_frame_of(const char *kind)
{
    char needle[32];
    (void)snprintf(needle, sizeof(needle), "{\"t\":\"%s\"", kind);
    const uint32_t n = g.cap.count < CAP_SLOTS ? g.cap.count : CAP_SLOTS;
    for (uint32_t i = 0u; i < n; ++i) {
        const uint32_t slot = (g.cap.count - 1u - i) % CAP_SLOTS;
        if (strncmp(g.cap.frame[slot], needle, strlen(needle)) == 0) {
            return g.cap.frame[slot];
        }
    }
    return NULL;
}

static void bring_to_ready(void)
{
    uint32_t steps = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_IDLE, truing_orch_run_auto(&g.orch, &g.op, 50u, &steps));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_READY, g.orch.state);
}

/* Steps WITHOUT the auto-operator, so the machine actually stops at the wait and a
 * wire client gets the chance to answer it. */
static truing_orch_step_t step_until_stopped(uint32_t max_steps)
{
    truing_orch_step_t s = TRUING_ORCH_ADVANCED;
    for (uint32_t i = 0u; i < max_steps; ++i) {
        s = truing_orch_step(&g.orch);
        if (s == TRUING_ORCH_WAITING_OPERATOR || s == TRUING_ORCH_TERMINAL || s == TRUING_ORCH_IDLE) {
            return s;
        }
    }
    return s;
}

/* ---- tests --------------------------------------------------------------------------- */

static void test_pump_drains_the_ring_as_frames(void)
{
    bring_to_ready();
    TEST_ASSERT_TRUE(truing_telemetry_ring_count(&g.ring) > 0u);
    const uint32_t sent = truing_wire_session_pump(&g.session, 0u);
    TEST_ASSERT_TRUE(sent > 0u);
    TEST_ASSERT_EQUAL_UINT16(0u, truing_telemetry_ring_count(&g.ring));
    TEST_ASSERT_EQUAL_UINT32(sent, g.session.events_sent);
    TEST_ASSERT_EQUAL_UINT32(0u, g.session.events_dropped);
    /* Every frame is a well-formed event frame. */
    const uint32_t n = sent < CAP_SLOTS ? sent : CAP_SLOTS;
    for (uint32_t i = 0u; i < n; ++i) {
        truing_json_doc_t d;
        TEST_ASSERT_TRUE(truing_json_doc_init(&d, g.cap.frame[i], strlen(g.cap.frame[i])));
        truing_json_value_t v;
        TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&d, "t", &v));
        TEST_ASSERT_TRUE(truing_json_str_equals(&v, "event"));
    }
}

static void test_pump_is_bounded(void)
{
    bring_to_ready();
    const uint16_t before = truing_telemetry_ring_count(&g.ring);
    TEST_ASSERT_TRUE(before > 3u);
    TEST_ASSERT_EQUAL_UINT32(3u, truing_wire_session_pump(&g.session, 3u));
    TEST_ASSERT_EQUAL_UINT16(before - 3u, truing_telemetry_ring_count(&g.ring));
}

static void test_a_refusing_sink_drops_rather_than_wedges(void)
{
    /* SPEC §12.2: telemetry is best-effort with no replay. A sink that cannot take a
     * frame must cost the control path nothing beyond the drop. */
    bring_to_ready();
    g.cap.refuse = true;
    const uint16_t queued = truing_telemetry_ring_count(&g.ring);
    TEST_ASSERT_EQUAL_UINT32(0u, truing_wire_session_pump(&g.session, 0u));
    TEST_ASSERT_EQUAL_UINT16(0u, truing_telemetry_ring_count(&g.ring));   /* drained, not retried */
    TEST_ASSERT_EQUAL_UINT32(queued, g.session.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(0u, g.session.events_sent);
}

static void test_get_current_state_answers_with_a_snapshot_and_an_ack(void)
{
    bring_to_ready();
    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"GET_CURRENT_STATE\",\"seq\":5}", 0u));
    /* The answer, then exactly one acknowledgement. */
    TEST_ASSERT_EQUAL_UINT32(2u, g.cap.count);
    const char *state = last_frame_of("state");
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(strstr(state, "\"current_state\":\"READY\""));
    const char *ack = last_frame();
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"t\":\"ack\""));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"seq\":5"));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"accepted\":true"));
}

static void test_get_provenance_answers_with_the_p6_record(void)
{
    bring_to_ready();
    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"GET_CURRENT_CYCLE_PROVENANCE\"}", 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, g.cap.count);
    /* The provenance frame exceeds the capture slot, so check the length the sink saw. */
    TEST_ASSERT_TRUE(g.cap.len[0] > CAP_BYTES);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"t\":\"provenance\"", g.cap.frame[0], 17);
}

static void test_intent_verdicts_ride_in_the_ack(void)
{
    bring_to_ready();
    g.cap.count = 0u;
    /* START_TRUING is admissible in READY. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"START_TRUING\",\"seq\":1}", 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, g.cap.count);
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"verdict\":\"ACCEPT\""));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"accepted\":true"));
    TEST_ASSERT_EQUAL_UINT32(1u, g.session.commands_accepted);

    /* A second START_TRUING is not: SPEC §12.3 admits it in READY only. The verdict is
     * the orchestrator's, reported through the ack rather than decided at the wire. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"START_TRUING\",\"seq\":2}", 0u));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"accepted\":false"));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"verdict\":\"REJECT_STATE\""));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"seq\":2"));
    TEST_ASSERT_EQUAL_UINT32(1u, g.session.commands_rejected);
}

static void test_every_frame_gets_exactly_one_ack_and_keeps_its_seq(void)
{
    bring_to_ready();
    g.cap.count = 0u;
    /* Rejected at the wire, not by the orchestrator — but the client still learns which
     * of its frames was refused. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_WAIT_ID,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"CONFIRM_POSITIONED\",\"seq\":77}", 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, g.cap.count);
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"wire\":\"MISSING_WAIT_ID\""));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"seq\":77"));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"verdict\":null"));

    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MALFORMED, truing_wire_session_handle(&g.session, "garbage", 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, g.cap.count);
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"wire\":\"MALFORMED\""));
}

/* The scenario SPEC §12.2 requires the current-state query for: a client that holds no
 * wait_id learns it from the snapshot and answers the wait — here done entirely through
 * wire frames, with the id parsed back out of the JSON rather than read from the struct. */
static void test_a_client_can_answer_a_wait_using_only_wire_frames(void)
{
    bring_to_ready();
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"START_TRUING\"}", 0u));

    /* Run until the machine stops at an operator wait. */
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_WAITING_OPERATOR, step_until_stopped(200u));

    /* The client asks what is going on, having seen none of the events. */
    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        truing_wire_session_handle(&g.session, "{\"cmd\":\"GET_CURRENT_STATE\"}", 0u));
    const char *state = last_frame_of("state");
    TEST_ASSERT_NOT_NULL(state);

    /* Parse the wait out of the frame, exactly as a UI would. */
    truing_json_doc_t d;
    TEST_ASSERT_TRUE(truing_json_doc_init(&d, state, strlen(state)));
    truing_json_value_t wait;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "active_wait", &wait));
    truing_json_doc_t wd;
    TEST_ASSERT_TRUE(truing_json_doc_init(&wd, wait.start, wait.len));
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NUMBER, truing_json_get(&wd, "wait_id", &v));
    const unsigned wait_id = (unsigned)v.number;
    TEST_ASSERT_TRUE(wait_id > 0u);
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&wd, "expected_intent", &v));
    char expected[48];
    TEST_ASSERT_TRUE(truing_json_copy_str(&v, expected, sizeof(expected)));

    /* Answer it with the id the snapshot supplied. */
    char frame[160];
    if (strcmp(expected, "SUBMIT_RUNOUT") == 0) {
        (void)snprintf(frame, sizeof(frame),
                       "{\"cmd\":\"%s\",\"wait_id\":%u,\"lateral_mm\":0,\"radial_mm\":0}", expected, wait_id);
    } else {
        (void)snprintf(frame, sizeof(frame), "{\"cmd\":\"%s\",\"wait_id\":%u}", expected, wait_id);
    }
    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, truing_wire_session_handle(&g.session, frame, 0u));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"accepted\":true"));

    /* Once the machine has CONSUMED that wait, the same frame is stale. The step matters:
     * a duplicate arriving while the very same wait is still open is idempotent — it
     * records the same answer twice — and SPEC §7.3 is about a delayed confirmation
     * satisfying a LATER wait, which is what the wait_id prevents. */
    (void)step_until_stopped(200u);
    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, truing_wire_session_handle(&g.session, frame, 0u));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"accepted\":false"));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"verdict\":\"REJECT_"));
}

static void test_abort_is_accepted_while_the_machine_operates(void)
{
    /* SPEC §12.3: ABORT is a safety intent, admissible in all operational states. */
    bring_to_ready();
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, truing_wire_session_handle(&g.session, "{\"cmd\":\"START_TRUING\"}", 0u));
    (void)step_until_stopped(200u);
    TEST_ASSERT_TRUE(truing_state_is_operational(g.orch.state));
    g.cap.count = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, truing_wire_session_handle(&g.session, "{\"cmd\":\"ABORT\"}", 0u));
    TEST_ASSERT_NOT_NULL(strstr(last_frame(), "\"accepted\":true"));
}

static void test_a_session_without_a_sink_still_rules_on_commands(void)
{
    /* The host may be gone; the state machine must not be. */
    bring_to_ready();
    TEST_ASSERT_TRUE(truing_wire_session_init(&g.session, &g.orch, &g.ring, NULL, false));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, truing_wire_session_handle(&g.session, "{\"cmd\":\"START_TRUING\"}", 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, g.session.commands_accepted);
    TEST_ASSERT_NOT_EQUAL_INT(TRUING_STATE_READY, g.orch.state);
    /* Pumping with no sink drops rather than crashing. */
    (void)truing_wire_session_pump(&g.session, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, g.session.events_sent);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_pump_drains_the_ring_as_frames);
    RUN_TEST(test_pump_is_bounded);
    RUN_TEST(test_a_refusing_sink_drops_rather_than_wedges);
    RUN_TEST(test_get_current_state_answers_with_a_snapshot_and_an_ack);
    RUN_TEST(test_get_provenance_answers_with_the_p6_record);
    RUN_TEST(test_intent_verdicts_ride_in_the_ack);
    RUN_TEST(test_every_frame_gets_exactly_one_ack_and_keeps_its_seq);
    RUN_TEST(test_a_client_can_answer_a_wait_using_only_wire_frames);
    RUN_TEST(test_abort_is_accepted_while_the_machine_operates);
    RUN_TEST(test_a_session_without_a_sink_still_rules_on_commands);
    return UNITY_END();
}
