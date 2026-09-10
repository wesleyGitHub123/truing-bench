/* Non-hardware implementations of the hardware-facing contracts (SPEC P2, §9.1, §10.1, §12.2,
 * §12.4, §14.5) and the session header's provenance flag (SPEC §6.2). */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "truing/session.h"
#include "truing/wheel_state.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/intent_source_if.h"
#include "truing_hal/pluck_if.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"

static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static truing_wheel_class_config_t g_wheel;

void setUp(void)
{
    truing_fake_clock_init(&g_fc, &g_clock, 1000u);
    truing_fixture_wheel_class_sym32(&g_wheel);
}
void tearDown(void) {}

static void test_fake_clock(void)
{
    TEST_ASSERT_EQUAL_UINT32(1000u, truing_clock_now_ms(&g_clock));
    truing_fake_clock_advance(&g_fc, 250u);
    TEST_ASSERT_EQUAL_UINT32(1250u, truing_clock_now_ms(&g_clock));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_clock_now_ms(NULL));
}

static void test_acoustic_stub_returns_status_not_a_number(void)
{
    truing_acoustic_if_t a;
    truing_acoustic_stub_ctx_t ctx;
    truing_acoustic_stub_init(&a, &ctx, g_clock);
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 3u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, e.meta.reason_code);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, e.meta.source_impl);
    TEST_ASSERT_TRUE(isnan(e.tension_n));   /* never 0.0 */
    TEST_ASSERT_EQUAL_UINT32(1000u, e.meta.timestamp_ms);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&e));
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.calls);
    /* Missing implementation slot -> same honest answer. */
    truing_acoustic_measure(NULL, 3u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    truing_acoustic_request_cancel(NULL);
}

static void test_acoustic_synthetic_is_provisional_and_storable(void)
{
    truing_acoustic_if_t a;
    truing_acoustic_synthetic_ctx_t ctx;
    truing_acoustic_synthetic_init(&a, &ctx, g_clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    ctx.snr_db = 28.0f;
    ctx.selection_rule_version = 1u;
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_acoustic_synthetic_set_spoke(&ctx, i, 900.0f + (float)i, 460.0f + (float)i);
    }
    truing_acoustic_synthetic_set_failure(&ctx, 5u, TRUING_STATUS_REJECTED, TRUING_REASON_NO_ONSET_DETECTED);

    truing_wheel_state_t ws;
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&ws, 32u, 32u, 2u));
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 7u, &g_wheel, 2u, &e);
    /* SPEC §4.4.1: suspect / PROVISIONAL_MODE_ID / presumed_fundamental while layer 3 is interim. */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PROVISIONAL_MODE_ID, e.meta.reason_code);
    TEST_ASSERT_EQUAL_INT(TRUING_MODE_ID_PRESUMED_FUNDAMENTAL, e.frequency.mode_identity);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, e.meta.source_impl);
    TEST_ASSERT_EQUAL_INT(TRUING_TENSION_MODEL_IDEAL_STRING, e.model_name);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 907.0f, e.tension_n);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 467.0f, e.frequency.selected_frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(1u, e.frequency.n_candidates);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&e));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&ws, 7u, &e, NULL));
    TEST_ASSERT_FALSE(truing_status_is_verification_grade(e.meta.status));
    TEST_ASSERT_TRUE(truing_status_is_solver_admissible(e.meta.status));

    truing_acoustic_measure(&a, 5u, &g_wheel, 2u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NO_ONSET_DETECTED, e.meta.reason_code);
    TEST_ASSERT_TRUE(isnan(e.tension_n));
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_spoke(&ws, 5u, &e, NULL));

    /* Out-of-range spoke and cooperative cancellation. */
    truing_acoustic_measure(&a, 32u, &g_wheel, 2u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_VALUE_OUT_OF_RANGE, e.meta.reason_code);
    truing_acoustic_request_cancel(&a);
    truing_acoustic_measure(&a, 1u, &g_wheel, 2u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CANCELLED, e.meta.reason_code);
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.cancelled_calls);
    truing_acoustic_measure(&a, 1u, &g_wheel, 2u, &e);   /* cancel flag is consumed */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    /* An unscripted spoke yields NaN and is rejected by validation — never a silent zero. */
    truing_acoustic_synthetic_ctx_t bare;
    truing_acoustic_if_t b;
    truing_acoustic_synthetic_init(&b, &bare, g_clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    truing_acoustic_measure(&b, 0u, &g_wheel, 2u, &e);
    TEST_ASSERT_NOT_EQUAL(TRUING_CHECK_OK, truing_tension_estimate_check(&e));
}

static void test_runout_stub_reports_every_capability_absent(void)
{
    truing_runout_if_t r;
    truing_runout_stub_ctx_t ctx;
    truing_runout_stub_init(&r, &ctx, g_clock);
    TEST_ASSERT_EQUAL_UINT32(0u, truing_runout_capabilities(&r));
    truing_runout_measurement_t m;
    truing_runout_read_snapshot(&r, 3u, 0.5f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, m.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, m.meta.reason_code);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, m.rim_angle_rad);   /* rim_angle is mandatory even here */
    TEST_ASSERT_TRUE(isnan(m.lateral_mm));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_runout_measurement_check(&m));
    truing_reason_t reason = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_runout_stream_samples(&r, NULL, NULL, 10u, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, reason);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_runout_tare(&r, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_runout_tare(NULL, &reason));
}

static bool never_called(const truing_runout_sample_t *s, void *user)
{
    (void)s;
    (void)user;
    return false;
}

static void test_runout_synthetic_snapshot_only(void)
{
    truing_runout_if_t r;
    truing_runout_synthetic_ctx_t ctx;
    truing_runout_synthetic_init(&r, &ctx, g_clock, 32u);
    TEST_ASSERT_EQUAL_UINT32(TRUING_RUNOUT_CAP_SNAPSHOT | TRUING_RUNOUT_CAP_TARE, truing_runout_capabilities(&r));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_runout_capabilities(&r) & TRUING_RUNOUT_CAP_STREAM);
    truing_runout_synthetic_set_index(&ctx, 0u, 0.10f, 0.05f);
    truing_runout_synthetic_set_index(&ctx, 1u, 0.25f, -0.01f);
    truing_runout_synthetic_set_failure(&ctx, 2u, TRUING_STATUS_REJECTED, TRUING_REASON_VALUE_OUT_OF_RANGE);

    truing_wheel_state_t ws;
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_init(&ws, 32u, 32u, 1u));
    truing_runout_measurement_t m;
    truing_runout_read_snapshot(&r, 1u, truing_wheel_state_rim_angle(&ws, 1u), 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, m.meta.status);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, m.lateral_mm);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&ws, 1u, &m, NULL));

    /* SPEC §10.3: tare zeroes the gauges at the reference (rim index 0). */
    truing_reason_t reason = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, truing_runout_tare(&r, &reason));
    truing_runout_read_snapshot(&r, 0u, 0.0f, 1u, &m);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, m.lateral_mm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, m.radial_mm);
    truing_runout_read_snapshot(&r, 1u, truing_wheel_state_rim_angle(&ws, 1u), 1u, &m);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.15f, m.lateral_mm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.06f, m.radial_mm);

    truing_runout_read_snapshot(&r, 2u, truing_wheel_state_rim_angle(&ws, 2u), 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, m.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_WS_OK, truing_wheel_state_set_runout(&ws, 2u, &m, NULL));

    /* SPEC §10.1: a snapshot-only device reports streaming unavailable, never a fabricated stream. */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, truing_runout_stream_samples(&r, never_called, NULL, 5u, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NOT_IMPLEMENTED, reason);
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.stream_calls);
    /* Unscripted index: NaN, rejected by validation, never zero. */
    truing_runout_read_snapshot(&r, 9u, truing_wheel_state_rim_angle(&ws, 9u), 1u, &m);
    TEST_ASSERT_NOT_EQUAL(TRUING_CHECK_OK, truing_runout_measurement_check(&m));
    truing_runout_read_snapshot(&r, 40u, 0.0f, 1u, &m);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_VALUE_OUT_OF_RANGE, m.meta.reason_code);
}

static void test_intent_queue_source(void)
{
    truing_intent_source_if_t src;
    truing_intent_queue_ctx_t ctx;
    truing_intent_queue_init(&src, &ctx);
    truing_intent_t in, out;
    memset(&in, 0, sizeof(in));
    TEST_ASSERT_FALSE(truing_intent_source_poll(&src, &out));
    for (uint32_t i = 0; i < TRUING_INTENT_QUEUE_CAPACITY; ++i) {
        in.type = TRUING_INTENT_ABORT;
        in.wait_id = i;
        TEST_ASSERT_TRUE(truing_intent_queue_push(&ctx, &in));
    }
    TEST_ASSERT_FALSE(truing_intent_queue_push(&ctx, &in));
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.dropped);
    TEST_ASSERT_EQUAL_UINT8(TRUING_INTENT_QUEUE_CAPACITY, truing_intent_queue_count(&ctx));
    TEST_ASSERT_TRUE(truing_intent_source_poll(&src, &out));
    TEST_ASSERT_EQUAL_UINT32(0u, out.wait_id);   /* FIFO */
    TEST_ASSERT_TRUE(truing_intent_source_poll(&src, &out));
    TEST_ASSERT_EQUAL_UINT32(1u, out.wait_id);
    TEST_ASSERT_FALSE(truing_intent_source_poll(NULL, &out));
}

static void test_telemetry_ring_drops_on_full_and_never_blocks(void)
{
    truing_telemetry_if_t sink;
    truing_telemetry_ring_ctx_t ctx;
    truing_telemetry_ring_init(&sink, &ctx);
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_STATE_TRANSITION;
    for (uint32_t i = 0; i < TRUING_TELEMETRY_RING_CAPACITY; ++i) {
        ev.timestamp_ms = i;
        TEST_ASSERT_TRUE(truing_telemetry_emit(&sink, &ev));
    }
    TEST_ASSERT_FALSE(truing_telemetry_emit(&sink, &ev));   /* dropped, not blocked */
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.dropped);
    TEST_ASSERT_EQUAL_UINT32(TRUING_TELEMETRY_RING_CAPACITY, ctx.emitted);
    truing_telemetry_event_t out;
    TEST_ASSERT_TRUE(truing_telemetry_ring_pop(&ctx, &out));
    TEST_ASSERT_EQUAL_UINT32(0u, out.timestamp_ms);
    TEST_ASSERT_EQUAL_UINT16(TRUING_TELEMETRY_RING_CAPACITY - 1u, truing_telemetry_ring_count(&ctx));
    /* Telemetry failure never affects control flow: a NULL sink simply drops. */
    TEST_ASSERT_FALSE(truing_telemetry_emit(NULL, &ev));
    ev.kind = TRUING_EVT_NAVIGATION;
    ev.u.navigation.station = (uint8_t)TRUING_STATION_RUNOUT;
    ev.u.navigation.outcome = 2u;   /* plain integer on the wire: the event carries no navigation types */
    TEST_ASSERT_TRUE(truing_telemetry_emit(&sink, &ev));
    TEST_ASSERT_EQUAL_STRING("NAVIGATION", truing_event_kind_str(TRUING_EVT_NAVIGATION));
}

static void test_session_header_flags_non_real_implementations(void)
{
    truing_solver_config_t solver;
    truing_fixture_solver_config(&solver, 32u);
    truing_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    fp.set = true;
    truing_session_header_t h;

    const truing_impl_descriptor_t all_real[] = {
        { "acoustic_inmp441", TRUING_SOURCE_REAL },
        { "runout_manual", TRUING_SOURCE_REAL },
        { "navigation_manual", TRUING_SOURCE_REAL },
    };
    TEST_ASSERT_TRUE(truing_session_header_init(&h, 42u, "0.1.0-test", &g_wheel, &solver, 1u, 1u, 7u, &fp,
                                                all_real, sizeof(all_real) / sizeof(all_real[0])));
    TEST_ASSERT_FALSE(h.contains_non_real_implementations);
    TEST_ASSERT_EQUAL_UINT32(42u, h.session_id);
    TEST_ASSERT_EQUAL_STRING("0.1.0-test", h.firmware_version);
    TEST_ASSERT_EQUAL_UINT32(solver.tension_model_profile_id, h.tension_model_profile_id);
    TEST_ASSERT_EQUAL_UINT32(1u, h.machine_profile_id);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, solver.target_tension_n, h.target_tension_n);
    TEST_ASSERT_EQUAL_UINT8(32u, h.wheel_class_snapshot.n_spokes);
    TEST_ASSERT_FALSE(h.wall_clock_known);

    /* One stub anywhere flags the whole session (SPEC §6.2). */
    const truing_impl_descriptor_t with_stub[] = {
        { "acoustic_stub", TRUING_SOURCE_SYNTHETIC },
        { "runout_manual", TRUING_SOURCE_REAL },
        { "navigation_manual", TRUING_SOURCE_REAL },
    };
    TEST_ASSERT_TRUE(truing_session_header_init(&h, 43u, "0.1.0-test", &g_wheel, &solver, 1u, 1u, 7u, &fp,
                                                with_stub, sizeof(with_stub) / sizeof(with_stub[0])));
    TEST_ASSERT_TRUE(h.contains_non_real_implementations);
    TEST_ASSERT_TRUE(truing_session_any_non_real(NULL, 1u));   /* unknown cannot be assumed real */

    /* SPEC §6.6: wall clock recorded once; boot-relative timestamps resolve afterwards. */
    int64_t unix_ms = 0;
    TEST_ASSERT_FALSE(truing_session_resolve_timestamp(&h, 5000u, &unix_ms));
    truing_session_header_set_wall_clock(&h, 1700000000000LL, 12000u);
    TEST_ASSERT_TRUE(truing_session_resolve_timestamp(&h, 5000u, &unix_ms));
    /* Compared as a boolean: the default Unity build has 64-bit assertions disabled. */
    TEST_ASSERT_TRUE(unix_ms == (1700000000000LL - 12000LL + 5000LL));
}

/* The fake actuator is the seam every self-play and host run commands through; its failure
 * injection existed from the start and is what lets the acoustic tests script a failed fire. */
static void test_pluck_fake_counts_commands_and_reports_failures(void)
{
    truing_pluck_if_t pl;
    truing_pluck_fake_ctx_t ctx;

    truing_pluck_fake_init(&pl, &ctx, false);
    TEST_ASSERT_EQUAL_STRING("pluck_fake", pl.impl_name);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, pl.source_impl);
    TEST_ASSERT_FALSE(pl.available(&pl));    /* detached: nothing is attached and ready */
    TEST_ASSERT_FALSE(pl.fire(&pl, 20.0f));  /* and nothing is commanded into the void */
    TEST_ASSERT_EQUAL_UINT32(0u, ctx.fires);

    truing_pluck_fake_init(&pl, &ctx, true);
    TEST_ASSERT_TRUE(pl.available(&pl));
    TEST_ASSERT_TRUE(pl.fire(&pl, 12.5f));
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.fires);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, ctx.last_pulse_ms);
    /* fail_next: a one-shot failure injection - the next command goes through again */
    ctx.fail_next = true;
    TEST_ASSERT_FALSE(pl.fire(&pl, 12.5f));
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.fires);
    TEST_ASSERT_TRUE(pl.fire(&pl, 20.0f));
    TEST_ASSERT_EQUAL_UINT32(2u, ctx.fires);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, ctx.last_pulse_ms);
    /* a non-positive width is refused, not clipped */
    TEST_ASSERT_FALSE(pl.fire(&pl, 0.0f));
    TEST_ASSERT_EQUAL_UINT32(2u, ctx.fires);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_fake_clock);
    RUN_TEST(test_pluck_fake_counts_commands_and_reports_failures);
    RUN_TEST(test_acoustic_stub_returns_status_not_a_number);
    RUN_TEST(test_acoustic_synthetic_is_provisional_and_storable);
    RUN_TEST(test_runout_stub_reports_every_capability_absent);
    RUN_TEST(test_runout_synthetic_snapshot_only);
    RUN_TEST(test_intent_queue_source);
    RUN_TEST(test_telemetry_ring_drops_on_full_and_never_blocks);
    RUN_TEST(test_session_header_flags_non_real_implementations);
    return UNITY_END();
}
