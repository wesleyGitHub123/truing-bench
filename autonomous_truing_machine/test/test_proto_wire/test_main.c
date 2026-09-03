/* SPEC §12 wire protocol: command decoding (including the §12.3 wait_id contract),
 * telemetry event frames, the required current-state query, GET_CURRENT_CYCLE_PROVENANCE,
 * and acknowledgements. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "truing/limits.h"
#include "truing_proto/json.h"
#include "truing_proto/wire.h"

static char g_buf[TRUING_WIRE_PROVENANCE_BUF];

void setUp(void)
{
    memset(g_buf, 0, sizeof(g_buf));
}
void tearDown(void) {}

static truing_wire_error_t decode(const char *s, truing_wire_command_t *out)
{
    return truing_wire_decode_command(s, 0u, out);
}

/* Every frame this protocol emits must survive its own parser: that is what proves
 * the writer produces structurally valid JSON and not merely plausible text. */
static void reparse(size_t len, truing_json_doc_t *d)
{
    TEST_ASSERT_NOT_EQUAL_size_t(0u, len);
    TEST_ASSERT_EQUAL_size_t(strlen(g_buf), len);
    TEST_ASSERT_TRUE(truing_json_doc_init(d, g_buf, len));
}

static void assert_str_field(const truing_json_doc_t *d, const char *key, const char *expect)
{
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(d, key, &v));
    TEST_ASSERT_TRUE(truing_json_str_equals(&v, expect));
}

static double num_field(const truing_json_doc_t *d, const char *key)
{
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NUMBER, truing_json_get(d, key, &v));
    return v.number;
}

/* ---- Decoding ---------------------------------------------------------------------- */

static void test_decode_simple_intents_and_queries(void)
{
    truing_wire_command_t c;

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"START_TRUING\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_CMD_INTENT, c.kind);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_START_TRUING, c.intent.type);
    TEST_ASSERT_EQUAL_UINT32(0u, c.intent.wait_id);
    TEST_ASSERT_FALSE(c.has_seq);

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"ABORT\",\"seq\":9}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ABORT, c.intent.type);
    TEST_ASSERT_TRUE(c.has_seq);
    TEST_ASSERT_EQUAL_UINT32(9u, c.seq);

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"GET_CURRENT_STATE\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_CMD_GET_CURRENT_STATE, c.kind);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_UNSET, c.intent.type);

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"GET_CURRENT_CYCLE_PROVENANCE\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_CMD_GET_CURRENT_CYCLE_PROVENANCE, c.kind);
}

static void test_decode_confirmations_carry_payloads(void)
{
    truing_wire_command_t c;

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"CONFIRM_POSITIONED\",\"wait_id\":4}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_CONFIRM_POSITIONED, c.intent.type);
    TEST_ASSERT_EQUAL_UINT32(4u, c.intent.wait_id);

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        decode("{\"cmd\":\"SUBMIT_RUNOUT\",\"wait_id\":11,\"lateral_mm\":0.31,\"radial_mm\":-0.12}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_SUBMIT_RUNOUT, c.intent.type);
    TEST_ASSERT_EQUAL_UINT32(11u, c.intent.wait_id);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.31f, c.intent.payload.runout.lateral_mm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.12f, c.intent.payload.runout.radial_mm);

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"CONFIRM_ADJUSTMENT_DONE\",\"wait_id\":12}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE, c.intent.type);
}

static void test_decode_set_parameter(void)
{
    truing_wire_command_t c;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"target_tension\",\"value\":1100.5}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_SET_PARAMETER, c.intent.type);
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_TARGET_TENSION, c.intent.payload.set_parameter.id);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1100.5f, c.intent.payload.set_parameter.value);

    /* Artifact-bound parameters are addressable and decode fine; refusing them with
     * REQUIRES_ARTIFACT_REGENERATION is a §12.3.1 admissibility decision, not a wire one. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"n_spokes\",\"value\":32}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_PARAM_N_SPOKES, c.intent.payload.set_parameter.id);

    /* Host-only model-preparation values have no parameter id at all, so SPEC §12.3.1's
     * "outside the command surface" is enforced by being unaddressable. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNKNOWN_PARAMETER,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"n_fit_samples_lat\",\"value\":8}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNKNOWN_PARAMETER,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"T_target_assumed\",\"value\":1000}", &c));
    /* The UNSET sentinel is not a parameter name either. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNKNOWN_PARAMETER,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"UNSET\",\"value\":1}", &c));

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_FIELD,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"target_tension\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_FIELD,
        decode("{\"cmd\":\"SET_PARAMETER\",\"value\":1}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_TYPE,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"target_tension\",\"value\":\"1100\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_TYPE,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":7,\"value\":1}", &c));
    /* A magnitude no float can hold is a range error, not a silent infinity. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_RANGE,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"target_tension\",\"value\":1e400}", &c));
    /* A rejected frame leaves nothing half-filled behind. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_CMD_UNSET, c.kind);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_UNSET, c.intent.type);
}

static void test_spec_12_3_wait_id_is_a_wire_contract(void)
{
    truing_wire_command_t c;

    /* Omitted on a confirmation: rejected here. There is nothing to correlate. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_WAIT_ID, decode("{\"cmd\":\"CONFIRM_POSITIONED\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_WAIT_ID, decode("{\"cmd\":\"CONFIRM_ADJUSTMENT_DONE\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_WAIT_ID,
        decode("{\"cmd\":\"SUBMIT_RUNOUT\",\"lateral_mm\":0,\"radial_mm\":0}", &c));

    /* Zero is the "not answering a wait" sentinel, so it identifies no wait either. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_WAIT_ID,
        decode("{\"cmd\":\"CONFIRM_POSITIONED\",\"wait_id\":0}", &c));

    /* Present but wrong is NOT judged here: only the orchestrator knows which wait is
     * active, and it answers STALE_INTENT. The wire layer passes the value through. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"CONFIRM_POSITIONED\",\"wait_id\":99999}", &c));
    TEST_ASSERT_EQUAL_UINT32(99999u, c.intent.wait_id);

    /* SPEC §12.3: "ABORT and START_TRUING carry no wait_id — they are not answers to a
     * wait." A client that sends one has misunderstood the protocol. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID, decode("{\"cmd\":\"ABORT\",\"wait_id\":3}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID, decode("{\"cmd\":\"START_TRUING\",\"wait_id\":1}", &c));
    /* SET_PARAMETER does not answer a wait either, even where it is admissible. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID,
        decode("{\"cmd\":\"SET_PARAMETER\",\"param\":\"max_cycles\",\"value\":3,\"wait_id\":1}", &c));

    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_TYPE,
        decode("{\"cmd\":\"CONFIRM_POSITIONED\",\"wait_id\":\"4\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_RANGE,
        decode("{\"cmd\":\"CONFIRM_POSITIONED\",\"wait_id\":1.5}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_RANGE,
        decode("{\"cmd\":\"CONFIRM_POSITIONED\",\"wait_id\":-1}", &c));
}

static void test_decode_rejects_bad_frames(void)
{
    truing_wire_command_t c;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MALFORMED, decode("not json", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MALFORMED, decode("[]", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MALFORMED, decode("{\"cmd\":\"ABORT\"", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MALFORMED, truing_wire_decode_command(NULL, 0u, &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_FIELD, decode("{}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_TYPE, decode("{\"cmd\":7}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNKNOWN_COMMAND, decode("{\"cmd\":\"REBOOT\"}", &c));
    /* UNSET is a sentinel, never a command. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_UNKNOWN_COMMAND, decode("{\"cmd\":\"UNSET\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_TYPE, decode("{\"cmd\":\"ABORT\",\"seq\":\"x\"}", &c));

    char big[TRUING_WIRE_COMMAND_MAX + 64u];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1u] = '\0';
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_TOO_LONG, decode(big, &c));
}

static void test_decode_debug_channel(void)
{
    truing_wire_command_t c;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"DEBUG\",\"code\":5,\"arg\":-2}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_DEBUG, c.intent.type);
    TEST_ASSERT_EQUAL_UINT16(5u, c.intent.payload.debug.code);
    TEST_ASSERT_EQUAL_INT32(-2, c.intent.payload.debug.arg);
    /* arg defaults to 0 */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"DEBUG\",\"code\":1}", &c));
    TEST_ASSERT_EQUAL_INT32(0, c.intent.payload.debug.arg);
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_MISSING_FIELD, decode("{\"cmd\":\"DEBUG\"}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_ERR_FIELD_RANGE, decode("{\"cmd\":\"DEBUG\",\"code\":70000}", &c));
}

/* ---- Encoding ---------------------------------------------------------------------- */

static void test_encode_state_transition_event(void)
{
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_STATE_TRANSITION;
    ev.timestamp_ms = 12345u;
    ev.cycle_index = 2u;
    ev.u.transition.from = TRUING_STATE_READY;
    ev.u.transition.to = TRUING_STATE_MEASURE_WHEEL_STATE;

    truing_json_doc_t d;
    reparse(truing_wire_encode_event(&ev, g_buf, sizeof(g_buf)), &d);
    assert_str_field(&d, "t", "event");
    assert_str_field(&d, "kind", "STATE_TRANSITION");
    assert_str_field(&d, "from", "READY");
    assert_str_field(&d, "to", "MEASURE_WHEEL_STATE");
    TEST_ASSERT_EQUAL_INT(12345, (int)num_field(&d, "ts_ms"));
    TEST_ASSERT_EQUAL_INT(2, (int)num_field(&d, "cycle_index"));
}

static void test_encode_measurement_events_name_their_channel(void)
{
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_MEASUREMENT_RESULT;
    ev.u.measurement.channel = TRUING_EVT_CHANNEL_TENSION;
    ev.u.measurement.index = 7u;
    ev.u.measurement.status = TRUING_STATUS_SUSPECT;
    ev.u.measurement.reason = TRUING_REASON_PROVISIONAL_MODE_ID;
    ev.u.measurement.value_a = 663.7f;
    ev.u.measurement.value_b = 423.886f;

    truing_json_doc_t d;
    reparse(truing_wire_encode_event(&ev, g_buf, sizeof(g_buf)), &d);
    /* The wire vocabulary is the firmware's own string table verbatim, case included:
     * statuses are lowercase and reason codes uppercase because that is how the
     * firmware names them everywhere else. Re-spelling them here would be the one way
     * the protocol and the logs could drift apart. */
    assert_str_field(&d, "channel", "TENSION");
    assert_str_field(&d, "status", "suspect");
    assert_str_field(&d, "reason", "PROVISIONAL_MODE_ID");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 663.7f, (float)num_field(&d, "tension_n"));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 423.886f, (float)num_field(&d, "selected_frequency_hz"));

    ev.u.measurement.channel = TRUING_EVT_CHANNEL_RUNOUT;
    ev.u.measurement.status = TRUING_STATUS_VALID;
    ev.u.measurement.reason = TRUING_REASON_NONE;
    ev.u.measurement.value_a = -0.25f;
    ev.u.measurement.value_b = 0.4f;
    reparse(truing_wire_encode_event(&ev, g_buf, sizeof(g_buf)), &d);
    assert_str_field(&d, "channel", "RUNOUT");
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.25f, (float)num_field(&d, "lateral_mm"));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.4f, (float)num_field(&d, "radial_mm"));
    /* The tension names must not appear on a runout event. */
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_ABSENT, truing_json_get(&d, "tension_n", &v));
}

static void test_encode_wait_prompt_names_feature_and_station(void)
{
    /* SPEC §12.2/§10A: a positioning prompt names the wheel feature AND its station. */
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_WAIT_ISSUED;
    ev.u.wait.wait_id = 6u;
    ev.u.wait.kind = TRUING_WAIT_POSITION_TO_SPOKE;
    ev.u.wait.expected_intent = TRUING_INTENT_CONFIRM_POSITIONED;
    ev.u.wait.station = TRUING_STATION_ACOUSTIC;
    ev.u.wait.target_index = 13u;

    size_t len = truing_wire_encode_event(&ev, g_buf, sizeof(g_buf));
    TEST_ASSERT_NOT_EQUAL_size_t(0u, len);
    truing_json_doc_t d;
    reparse(len, &d);
    /* The prompt is a nested object; check it survived and carries both facts. */
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"station\":\"acoustic\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"target_index\":13"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"kind\":\"POSITION_TO_SPOKE\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"expected_intent\":\"CONFIRM_POSITIONED\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"wait_id\":6"));
}

static void test_encode_navigation_event_renders_unvouched_rotation_as_null(void)
{
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_NAVIGATION;
    ev.u.navigation.target_kind = (uint8_t)TRUING_NAV_TARGET_SPOKE;
    ev.u.navigation.index = 4u;
    ev.u.navigation.angle_rad = 1.5f;
    ev.u.navigation.station = (uint8_t)TRUING_STATION_ADJUSTMENT;
    ev.u.navigation.outcome = (uint8_t)TRUING_NAV_FAULT;
    ev.u.navigation.rotation_rad = NAN;

    truing_json_doc_t d;
    reparse(truing_wire_encode_event(&ev, g_buf, sizeof(g_buf)), &d);
    assert_str_field(&d, "outcome", "FAULT");
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "rotation_rad", &v));
}

static void test_encode_log_event_bounds_untermined_text(void)
{
    /* The event's text field is a fixed array with no guaranteed NUL. */
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_LOG;
    memset(ev.u.text, 'A', TRUING_EVT_TEXT_MAX);
    truing_json_doc_t d;
    reparse(truing_wire_encode_event(&ev, g_buf, sizeof(g_buf)), &d);
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&d, "text", &v));
    TEST_ASSERT_EQUAL_size_t(TRUING_EVT_TEXT_MAX, v.len);
}

static void test_encode_event_drops_rather_than_truncates(void)
{
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_STATE_TRANSITION;
    char tiny[20];
    /* SPEC §12.2: telemetry is best-effort. Losing a frame is correct; half of one is not. */
    TEST_ASSERT_EQUAL_size_t(0u, truing_wire_encode_event(&ev, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

static void test_encode_state_snapshot(void)
{
    truing_orch_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.state = TRUING_STATE_WAIT_FOR_OPERATOR;
    s.waiting = true;
    s.active_wait.wait_id = 21u;
    s.active_wait.kind = TRUING_WAIT_ENTER_RUNOUT;
    s.active_wait.expected_intent = TRUING_INTENT_SUBMIT_RUNOUT;
    s.active_wait.station = TRUING_STATION_RUNOUT;
    s.active_wait.target_index = 5u;
    s.cycle_index = 1u;
    s.cycles_run = 1u;
    s.session_active = true;
    s.last_result = TRUING_TERMINAL_NONE;
    s.last_reason = TRUING_REASON_NONE;

    truing_json_doc_t d;
    reparse(truing_wire_encode_state(&s, g_buf, sizeof(g_buf)), &d);
    assert_str_field(&d, "t", "state");
    assert_str_field(&d, "current_state", "WAIT_FOR_OPERATOR");
    TEST_ASSERT_EQUAL_INT(1, (int)num_field(&d, "cycle_index"));
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "active_wait", &v));
    /* Everything a reconnecting UI needs to answer the wait it never saw announced. */
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"wait_id\":21"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"expected_intent\":\"SUBMIT_RUNOUT\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"station\":\"runout\""));
    /* No terminal result yet: null, not a fabricated value. */
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "last_known_result", &v));
}

static void test_encode_state_snapshot_without_a_wait(void)
{
    truing_orch_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.state = TRUING_STATE_TERMINAL;
    s.waiting = false;
    s.last_result = TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY;
    s.last_reason = TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE;

    truing_json_doc_t d;
    reparse(truing_wire_encode_state(&s, g_buf, sizeof(g_buf)), &d);
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "active_wait", &v));
    assert_str_field(&d, "last_known_result", "CONVERGED_GEOMETRIC_ONLY");
    assert_str_field(&d, "last_reason", "TENSION_NOT_VERIFICATION_GRADE");
}

static void test_encode_provenance_carries_the_p6_material(void)
{
    truing_cycle_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.session_id = 3u;
    p.artifact_id = 0xA5A5u;
    p.generating_fingerprint.set = true;
    for (unsigned i = 0u; i < TRUING_FINGERPRINT_BYTES; ++i) {
        p.generating_fingerprint.bytes[i] = (uint8_t)i;
    }
    p.tension_model_profile_id = 11u;
    p.chain_profile_id = 12u;
    p.machine_profile_id = 13u;
    p.active_layout = TRUING_LAYOUT_FULL;
    truing_row_mask_clear(&p.active_row_set);
    truing_row_mask_set(&p.active_row_set, 0u);
    truing_row_mask_set(&p.active_row_set, 9u);
    p.solver_config.tol_lateral_mm = 0.25f;
    p.solver_config.target_tension_n = 1100.0f;
    p.solver_config.trust_tension = 0.5f;
    p.wheel_summary.n_spokes = 32u;
    p.wheel_summary.n_rim_angles = 32u;
    p.wheel_summary.spokes_solver_admissible = 30u;
    p.wheel_summary.spokes_verification_grade = 0u;
    for (unsigned i = 0u; i < 32u; ++i) {
        p.spoke_status[i] = TRUING_STATUS_SUSPECT;
        p.spoke_reason[i] = TRUING_REASON_PROVISIONAL_MODE_ID;
        p.runout_status[i] = TRUING_STATUS_VALID;
        p.runout_reason[i] = TRUING_REASON_NONE;
    }
    p.plan.valid = true;
    p.plan.n_spokes = 32u;
    p.plan.n_suspect_rows = 32u;
    p.plan.policy_reason = TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE;
    p.plan.turns_rev[0] = 0.25f;
    p.plan.skipped[1] = true;
    p.wheel_position.status = TRUING_STATUS_VALID;
    p.wheel_position.reference_established = true;
    p.wheel_position.rotation_rad = 0.7854f;
    p.wheel_position.operator_confirmed = true;
    p.contains_non_real_implementations = true;

    const size_t len = truing_wire_encode_provenance(&p, g_buf, sizeof(g_buf));
    truing_json_doc_t d;
    reparse(len, &d);
    assert_str_field(&d, "t", "provenance");
    TEST_ASSERT_EQUAL_INT(0xA5A5, (int)num_field(&d, "influence_artifact_id"));
    assert_str_field(&d, "generating_fingerprint",
                     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    TEST_ASSERT_EQUAL_INT(11, (int)num_field(&d, "tension_model_profile_id"));
    TEST_ASSERT_EQUAL_INT(12, (int)num_field(&d, "chain_profile_id"));
    TEST_ASSERT_EQUAL_INT(13, (int)num_field(&d, "machine_profile_id"));
    /* rows 0 and 9 set: byte 0 bit 0, byte 1 bit 1 -> "0102..." */
    assert_str_field(&d, "active_row_set", "01020000000000000000000000000000");

    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "config_applied", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "wheel_state_summary", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_ARRAY, truing_json_get(&d, "spoke_status", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_ARRAY, truing_json_get(&d, "spoke_reason", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_ARRAY, truing_json_get(&d, "runout_status", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "plan", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "wheel_position", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_BOOL, truing_json_get(&d, "contains_non_real_implementations", &v));
    TEST_ASSERT_TRUE(v.boolean);
    /* The policy reason that explains why this plan is geometry-only. */
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"policy_reason\":\"MEAN_TENSION_MODEL_UNAVAILABLE\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"turns_rev\":[0.2500,"));
}

static void test_encode_provenance_without_a_fingerprint(void)
{
    truing_cycle_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.wheel_summary.n_spokes = 1u;
    p.wheel_summary.n_rim_angles = 1u;
    truing_json_doc_t d;
    reparse(truing_wire_encode_provenance(&p, g_buf, sizeof(g_buf)), &d);
    truing_json_value_t v;
    /* An unset fingerprint is null, never 32 zero bytes that could be mistaken for one. */
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "generating_fingerprint", &v));
}

static void test_provenance_fits_a_full_wheel_in_its_budget(void)
{
    /* The declared buffer must hold the largest wheel with the longest reason strings. */
    truing_cycle_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.generating_fingerprint.set = true;
    p.wheel_summary.n_spokes = TRUING_MAX_SPOKES;
    p.wheel_summary.n_rim_angles = TRUING_MAX_RIM_ANGLES;
    p.plan.valid = true;
    p.plan.n_spokes = TRUING_MAX_SPOKES;
    for (unsigned i = 0u; i < TRUING_MAX_SPOKES; ++i) {
        p.spoke_status[i] = TRUING_STATUS_UNAVAILABLE;
        p.spoke_reason[i] = TRUING_REASON_REQUIRES_ARTIFACT_REGENERATION;
        p.plan.turns_rev[i] = -1.2345f;
    }
    for (unsigned i = 0u; i < TRUING_MAX_RIM_ANGLES; ++i) {
        p.runout_status[i] = TRUING_STATUS_UNAVAILABLE;
        p.runout_reason[i] = TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE;
    }
    char full[TRUING_WIRE_PROVENANCE_BUF];
    const size_t len = truing_wire_encode_provenance(&p, full, sizeof(full));
    TEST_ASSERT_NOT_EQUAL_size_t(0u, len);
    TEST_ASSERT_LESS_THAN_size_t(sizeof(full), len);
    truing_json_doc_t d;
    TEST_ASSERT_TRUE(truing_json_doc_init(&d, full, len));
}

static void test_encode_ack(void)
{
    truing_wire_command_t c;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"START_TRUING\",\"seq\":42}", &c));

    truing_json_doc_t d;
    reparse(truing_wire_encode_ack(&c, TRUING_WIRE_OK, TRUING_INTENT_ADMIT_ACCEPT,
                                  TRUING_REASON_NONE, g_buf, sizeof(g_buf)), &d);
    assert_str_field(&d, "t", "ack");
    TEST_ASSERT_EQUAL_INT(42, (int)num_field(&d, "seq"));
    assert_str_field(&d, "intent", "START_TRUING");
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_BOOL, truing_json_get(&d, "accepted", &v));
    TEST_ASSERT_TRUE(v.boolean);

    /* Rejected by the orchestrator: the verdict and its reason are reported. */
    reparse(truing_wire_encode_ack(&c, TRUING_WIRE_OK, TRUING_INTENT_REJECT_STALE_INTENT,
                                  TRUING_REASON_STALE_INTENT, g_buf, sizeof(g_buf)), &d);
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_BOOL, truing_json_get(&d, "accepted", &v));
    TEST_ASSERT_FALSE(v.boolean);
    assert_str_field(&d, "verdict", "REJECT_STALE_INTENT");
    assert_str_field(&d, "reason", "STALE_INTENT");

    /* Rejected at the wire layer: no verdict exists to report, so it is null rather
     * than a default that would read as an orchestrator decision. */
    truing_wire_command_t bad;
    const truing_wire_error_t e = decode("{\"cmd\":\"CONFIRM_POSITIONED\"}", &bad);
    reparse(truing_wire_encode_ack(&bad, e, TRUING_INTENT_ADMIT_ACCEPT,
                                  TRUING_REASON_NONE, g_buf, sizeof(g_buf)), &d);
    assert_str_field(&d, "wire", "MISSING_WAIT_ID");
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "verdict", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_BOOL, truing_json_get(&d, "accepted", &v));
    TEST_ASSERT_FALSE(v.boolean);
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "seq", &v));
}

/* ---- The wire contract meeting the state machine ------------------------------------ */

static void test_decoded_confirmation_drives_the_correlator(void)
{
    /* A decoded frame goes to the orchestrator's admissibility unexamined: the wire
     * layer never duplicates a §12.3 admissibility rule, and STALE_INTENT is decided
     * where the active wait actually lives. */
    truing_wait_correlator_t wc;
    truing_wait_correlator_init(&wc);
    truing_wait_prompt_t req;
    memset(&req, 0, sizeof(req));
    req.kind = TRUING_WAIT_APPLY_ADJUSTMENT;
    req.station = TRUING_STATION_ADJUSTMENT;
    req.target_index = 3u;
    req.display_turns_rev = 0.25f;
    const uint32_t id = truing_wait_issue(&wc, &req);
    TEST_ASSERT_EQUAL_UINT32(1u, id);

    char frame[96];
    (void)snprintf(frame, sizeof(frame), "{\"cmd\":\"CONFIRM_ADJUSTMENT_DONE\",\"wait_id\":%u}", (unsigned)id);
    truing_wire_command_t c;
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode(frame, &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT,
        truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &wc, &c.intent, false));

    /* A well-formed frame echoing a wait that is no longer active is STALE_INTENT —
     * the orchestrator's answer, not the decoder's. */
    TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode("{\"cmd\":\"CONFIRM_ADJUSTMENT_DONE\",\"wait_id\":77}", &c));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STALE_INTENT,
        truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &wc, &c.intent, false));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_STALE_INTENT,
        truing_intent_verdict_reason(TRUING_INTENT_REJECT_STALE_INTENT));
}

static void test_every_intent_name_round_trips(void)
{
    /* The wire vocabulary is the enum's own string table, so every intent must decode
     * from the name the firmware prints for it. */
    for (unsigned i = 1u; i < (unsigned)TRUING_INTENT__COUNT; ++i) {
        const truing_intent_type_t t = (truing_intent_type_t)i;
        char frame[160];
        switch (t) {
        case TRUING_INTENT_SET_PARAMETER:
            (void)snprintf(frame, sizeof(frame),
                           "{\"cmd\":\"%s\",\"param\":\"max_cycles\",\"value\":4}", truing_intent_type_str(t));
            break;
        case TRUING_INTENT_SUBMIT_RUNOUT:
            (void)snprintf(frame, sizeof(frame),
                           "{\"cmd\":\"%s\",\"wait_id\":1,\"lateral_mm\":0,\"radial_mm\":0}",
                           truing_intent_type_str(t));
            break;
        case TRUING_INTENT_CONFIRM_POSITIONED:
        case TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE:
            (void)snprintf(frame, sizeof(frame), "{\"cmd\":\"%s\",\"wait_id\":1}", truing_intent_type_str(t));
            break;
        case TRUING_INTENT_DEBUG:
            (void)snprintf(frame, sizeof(frame), "{\"cmd\":\"%s\",\"code\":0}", truing_intent_type_str(t));
            break;
        default:
            (void)snprintf(frame, sizeof(frame), "{\"cmd\":\"%s\"}", truing_intent_type_str(t));
            break;
        }
        truing_wire_command_t c;
        TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode(frame, &c));
        TEST_ASSERT_EQUAL_INT(t, c.intent.type);
    }
}

static void test_every_parameter_name_round_trips(void)
{
    for (unsigned i = 1u; i < (unsigned)TRUING_PARAM__COUNT; ++i) {
        const truing_param_id_t id = (truing_param_id_t)i;
        char frame[160];
        (void)snprintf(frame, sizeof(frame),
                       "{\"cmd\":\"SET_PARAMETER\",\"param\":\"%s\",\"value\":1}", truing_param_str(id));
        truing_wire_command_t c;
        TEST_ASSERT_EQUAL_INT(TRUING_WIRE_OK, decode(frame, &c));
        TEST_ASSERT_EQUAL_INT(id, c.intent.payload.set_parameter.id);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_decode_simple_intents_and_queries);
    RUN_TEST(test_decode_confirmations_carry_payloads);
    RUN_TEST(test_decode_set_parameter);
    RUN_TEST(test_spec_12_3_wait_id_is_a_wire_contract);
    RUN_TEST(test_decode_rejects_bad_frames);
    RUN_TEST(test_decode_debug_channel);
    RUN_TEST(test_encode_state_transition_event);
    RUN_TEST(test_encode_measurement_events_name_their_channel);
    RUN_TEST(test_encode_wait_prompt_names_feature_and_station);
    RUN_TEST(test_encode_navigation_event_renders_unvouched_rotation_as_null);
    RUN_TEST(test_encode_log_event_bounds_untermined_text);
    RUN_TEST(test_encode_event_drops_rather_than_truncates);
    RUN_TEST(test_encode_state_snapshot);
    RUN_TEST(test_encode_state_snapshot_without_a_wait);
    RUN_TEST(test_encode_provenance_carries_the_p6_material);
    RUN_TEST(test_encode_provenance_without_a_fingerprint);
    RUN_TEST(test_provenance_fits_a_full_wheel_in_its_budget);
    RUN_TEST(test_encode_ack);
    RUN_TEST(test_decoded_confirmation_drives_the_correlator);
    RUN_TEST(test_every_intent_name_round_trips);
    RUN_TEST(test_every_parameter_name_round_trips);
    return UNITY_END();
}
