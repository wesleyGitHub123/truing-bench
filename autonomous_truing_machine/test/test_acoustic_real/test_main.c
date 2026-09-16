/* The real acoustic subsystem behind the one-call contract, driven by non-hardware front ends
 * (SPEC 9.1, 9.2, 14.5): status rules, format enforcement, cancellation, overrun, and the complete
 * workflow with the real calculation and the real acoustic layers 2-4 on a synthetic front end. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing/artifact.h"
#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/pluck_if.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/auto_operator.h"
#include "truing_orch/orchestrator.h"

static truing_chain_profile_t g_chain;
static truing_excitation_profile_t g_excitation;
static truing_tension_model_profile_t g_profile;
/* A fake actuator at each acoustic station, attached: what every measurement test excites with. */
static truing_pluck_if_t g_pl[TRUING_ACOUSTIC_STATIONS];
static truing_pluck_fake_ctx_t g_pctx[TRUING_ACOUSTIC_STATIONS];
static truing_acoustic_actuators_t g_act;
static truing_wheel_class_config_t g_wheel;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static void *g_scratch;
static size_t g_scratch_bytes;

void setUp(void)
{
    truing_fixture_chain_profile_inmp441(&g_chain);
    truing_fixture_excitation_profile(&g_excitation);
    truing_fixture_tension_model_profile_complete(&g_profile);
    for (unsigned i = 0; i < TRUING_ACOUSTIC_STATIONS; ++i) {
        truing_pluck_fake_init(&g_pl[i], &g_pctx[i], true);
        g_act.at[i] = &g_pl[i];
    }
    truing_fixture_wheel_class_sym32(&g_wheel);
    truing_fake_clock_init(&g_fc, &g_clock, 10u);
    g_scratch_bytes = truing_acoustic_real_scratch_bytes(&g_chain);
    g_scratch = malloc(g_scratch_bytes);
    TEST_ASSERT_NOT_NULL(g_scratch);
}
void tearDown(void)
{
    free(g_scratch);
    g_scratch = NULL;
}

static void test_synthetic_pluck_yields_a_provisional_estimate_through_all_four_layers(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    sctx.f2_hz = 2.03f * 480.0f;    /* a slightly inharmonic partner, as a stiff string has */
    sctx.amp2_rel = 0.3f;
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, a.source_impl);
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 3u, &g_wheel, 1u, &e);   /* spoke 3 is the RIGHT station's */
    TEST_ASSERT_EQUAL_UINT32(0u, g_pctx[0].fires);
    TEST_ASSERT_EQUAL_UINT32(1u, g_pctx[1].fires);
    TEST_ASSERT_EQUAL_FLOAT(g_excitation.pulse_ms[1], g_pctx[1].last_pulse_ms);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PROVISIONAL_MODE_ID, e.meta.reason_code);
    TEST_ASSERT_EQUAL_INT(TRUING_MODE_ID_PRESUMED_FUNDAMENTAL, e.frequency.mode_identity);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 480.0f, e.frequency.selected_frequency_hz);
    TEST_ASSERT_TRUE(e.frequency.snr_db > g_chain.measurement_min_snr_db);
    TEST_ASSERT_TRUE(e.frequency.n_candidates >= 2u);
    const float mu = g_profile.linear_density_kg_per_m.value, L = g_profile.L_eff_m.value;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * e.tension_n, 4.0f * mu * L * L * 480.0f * 480.0f, e.tension_n);
    TEST_ASSERT_EQUAL_INT(TRUING_TENSION_MODEL_IDEAL_STRING, e.model_name);
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&e));
    TEST_ASSERT_TRUE(truing_status_is_solver_admissible(e.meta.status));
    TEST_ASSERT_FALSE(truing_status_is_verification_grade(e.meta.status));
    TEST_ASSERT_EQUAL_UINT32(1u, ctx.diag.onsets);
    /* the two-mode model finds the partner and recovers a length near the profile's span */
    truing_tension_model_profile_t p = g_profile;
    p.model_name = TRUING_TENSION_MODEL_HIGHER_MODE;
    ctx.profile = &p;
    truing_acoustic_measure(&a, 3u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_TRUE(isfinite(ctx.diag.f2_hz));
    TEST_ASSERT_TRUE(e.tension_n > 0.0f);
    printf("synthetic pluck 480 Hz + 2.03x partner: f1 %.3f f2 %.2f snr %.1f dB M0 %.1f N M2 %.1f N (L_eff %.4f m)\n",
           (double)ctx.diag.f1_hz, (double)ctx.diag.f2_hz, (double)ctx.diag.snr_db, (double)(4.0f * mu * L * L * 480.0f * 480.0f),
           (double)e.tension_n, (double)ctx.diag.l_eff_m);
}

/* ---- capture evidence (SPEC §12.5) --------------------------------------------------------
 *
 * The claim the whole capture->fixture->replay seam rests on: the words a measurement is
 * handed out ARE the words it analysed, so replaying them reproduces that measurement exactly
 * rather than approximately. If this ever stops holding, every fixture taken off a board stops
 * being evidence about the board, and there is no way to notice from the fixture itself. */
static void test_capture_evidence_replays_to_the_same_measurement(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 465.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));

    /* Nothing measured yet: there is nothing to hand out, and it says so rather than
     * handing out an uninitialised buffer. */
    truing_acoustic_capture_view_t v;
    TEST_ASSERT_FALSE(truing_acoustic_real_last_capture(&a, &v));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_acoustic_real_capture_seq(&a));

    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 5u, &g_wheel, 2u, &e);
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    TEST_ASSERT_EQUAL_UINT32(1u, v.seq);
    TEST_ASSERT_EQUAL_UINT32(ctx.diag.n_captured, v.n_words);
    TEST_ASSERT_TRUE(v.n_words > 0u);
    TEST_ASSERT_EQUAL_PTR(ctx.words, v.words);
    /* the dump says which measurement it is evidence of, and what that measurement concluded */
    TEST_ASSERT_EQUAL_UINT8(5u, v.spoke_id);
    TEST_ASSERT_EQUAL_UINT8(2u, v.cycle_index);
    TEST_ASSERT_EQUAL_UINT32(1u, v.attempt);
    TEST_ASSERT_EQUAL_INT(e.meta.status, v.status);
    TEST_ASSERT_EQUAL_INT(e.meta.reason_code, v.reason);
    /* and which station's actuator excited them, with what pulse */
    TEST_ASSERT_EQUAL_UINT8(TRUING_STATION_ACOUSTIC_RIGHT, v.diag.station);
    TEST_ASSERT_TRUE(v.diag.fired);
    TEST_ASSERT_EQUAL_FLOAT(g_excitation.pulse_ms[1], v.diag.pulse_ms);
    /* pluck_fake reports too (deterministic: the commanded width, exactly) - this is the same
     * fire_report seam pluck_gpio uses to report its hardware-timed measurement on target. */
    TEST_ASSERT_TRUE(v.diag.pulse_measured);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(g_excitation.pulse_ms[1] * 1000.0f), v.diag.pulse_us_measured);

    /* Take the evidence away exactly as a dump would, then replay it. */
    const uint32_t n = v.n_words;
    int32_t *words = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    TEST_ASSERT_NOT_NULL(words);
    memcpy(words, v.words, (size_t)n * sizeof(int32_t));
    const truing_acoustic_real_diag_t d0 = v.diag;

    /* A retry on the same spoke overwrites the buffer, which is why seq exists. */
    truing_acoustic_measure(&a, 5u, &g_wheel, 2u, &e);
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    TEST_ASSERT_EQUAL_UINT32(2u, v.seq);
    TEST_ASSERT_EQUAL_UINT32(2u, v.attempt);

    truing_audio_source_if_t rsrc;
    truing_audio_buffer_ctx_t rctx;
    truing_audio_buffer_init(&rsrc, &rctx, words, n, NULL);
    truing_acoustic_if_t b;
    truing_acoustic_real_ctx_t bctx;
    void *bscratch = malloc(g_scratch_bytes);
    TEST_ASSERT_NOT_NULL(bscratch);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&b, &bctx, g_clock, &g_chain, &g_excitation, &g_profile, &rsrc, &g_act, bscratch, g_scratch_bytes, &detail));
    truing_tension_estimate_t re;
    truing_acoustic_real_analyze_words(&b, words, n, 2u, &re);
    const truing_acoustic_real_diag_t *d1 = &bctx.diag;

    /* Segmentation, gating and spectrum reproduce exactly — same code, same bytes. */
    TEST_ASSERT_EQUAL_UINT32(d0.n_captured, d1->n_captured);
    TEST_ASSERT_EQUAL_UINT32(d0.onsets, d1->onsets);
    TEST_ASSERT_EQUAL_UINT32(d0.onset_sample, d1->onset_sample);
    TEST_ASSERT_EQUAL_UINT32(d0.window.start_sample, d1->window.start_sample);
    TEST_ASSERT_EQUAL_UINT32(d0.window.n_samples, d1->window.n_samples);
    TEST_ASSERT_EQUAL_UINT32(d0.n_fft, d1->n_fft);
    TEST_ASSERT_EQUAL_UINT32(d0.n_strong_peaks, d1->n_strong_peaks);
    TEST_ASSERT_EQUAL_UINT32(d0.n_peaks_in_band, d1->n_peaks_in_band);
    TEST_ASSERT_EQUAL_FLOAT(d0.f1_hz, d1->f1_hz);
    TEST_ASSERT_EQUAL_FLOAT(d0.snr_db, d1->snr_db);
    /* and so does the conclusion drawn from them */
    TEST_ASSERT_EQUAL_INT(e.meta.status, re.meta.status);
    TEST_ASSERT_EQUAL_INT(e.meta.reason_code, re.meta.reason_code);
    TEST_ASSERT_EQUAL_FLOAT(e.tension_n, re.tension_n);
    /* the replay itself is a capture, and is labelled as one */
    TEST_ASSERT_EQUAL_UINT32(1u, truing_acoustic_real_capture_seq(&b));

    /* A subsystem that is not this implementation has nothing to show. */
    truing_acoustic_if_t sim;
    truing_acoustic_synthetic_ctx_t simctx;
    truing_acoustic_synthetic_init(&sim, &simctx, g_clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    TEST_ASSERT_FALSE(truing_acoustic_real_last_capture(&sim, &v));

    free(bscratch);
    free(words);
}

/* ---- phase observation -------------------------------------------------------------------
 *
 * One acoustic call is a window to pluck into and then seconds of arithmetic, and from outside
 * the subsystem those were indistinguishable. These pin the lifecycle the station is shown:
 * that it is emitted in order, that it says which spoke and which attempt, that it costs the
 * measurement nothing, and above all that no observer is REQUIRED - a build with none, or a
 * transport that drops every frame, must measure identically. */
#define OBS_MAX 32u
typedef struct {
    truing_telemetry_event_t ev[OBS_MAX];
    unsigned n;
    unsigned non_phase;
} obs_t;
static obs_t g_obs;

static void obs_fn(void *ctx, const truing_telemetry_event_t *event)
{
    obs_t *o = (obs_t *)ctx;
    if (event->kind != TRUING_EVT_ACOUSTIC_PHASE) {
        o->non_phase++;
        return;
    }
    if (o->n < OBS_MAX) {
        o->ev[o->n++] = *event;
    }
}
static unsigned obs_count(truing_acoustic_phase_t p)
{
    unsigned n = 0u;
    for (unsigned i = 0u; i < g_obs.n; ++i) {
        if (g_obs.ev[i].u.acoustic.phase == (uint8_t)p) n++;
    }
    return n;
}

static void test_phase_events_report_the_measurement_lifecycle_in_order(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);

    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 7u, &g_wheel, 2u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);

    /* Exactly the three boundaries, in the order they happen. */
    TEST_ASSERT_EQUAL_UINT32(3u, g_obs.n);
    TEST_ASSERT_EQUAL_UINT8(TRUING_ACOUSTIC_PHASE_LISTENING, g_obs.ev[0].u.acoustic.phase);
    TEST_ASSERT_EQUAL_UINT8(TRUING_ACOUSTIC_PHASE_ONSET_DETECTED, g_obs.ev[1].u.acoustic.phase);
    TEST_ASSERT_EQUAL_UINT8(TRUING_ACOUSTIC_PHASE_ANALYZING, g_obs.ev[2].u.acoustic.phase);
    TEST_ASSERT_EQUAL_UINT32(0u, g_obs.non_phase);
    for (unsigned i = 0u; i < g_obs.n; ++i) {
        TEST_ASSERT_EQUAL_INT(TRUING_EVT_ACOUSTIC_PHASE, g_obs.ev[i].kind);
        TEST_ASSERT_EQUAL_UINT8(7u, g_obs.ev[i].u.acoustic.spoke_index);
        TEST_ASSERT_EQUAL_UINT8(2u, g_obs.ev[i].cycle_index);
        TEST_ASSERT_EQUAL_UINT32(1u, g_obs.ev[i].u.acoustic.attempt);
    }
    /* Only LISTENING carries a window: the live capture, since the pre-trigger is already in the
     * ring when the call begins. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)g_chain.capture_ms, g_obs.ev[0].u.acoustic.window_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, g_obs.ev[1].u.acoustic.window_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, g_obs.ev[2].u.acoustic.window_ms);
    /* Spoke 7 is the RIGHT station's, and its actuator was commanded before the window opened. */
    for (unsigned i = 0u; i < g_obs.n; ++i) {
        TEST_ASSERT_EQUAL_UINT8(TRUING_STATION_ACOUSTIC_RIGHT, g_obs.ev[i].u.acoustic.station);
        TEST_ASSERT_TRUE(g_obs.ev[i].u.acoustic.fired);
    }
    src.close(&src);
}

/* A failed attempt still opens a window, and says which attempt it is - that is the whole point:
 * the orchestrator's silent retry (SPEC §7.4) was invisible from the station. */
static void test_a_silent_capture_reports_listening_but_never_an_onset(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.0f, 0.25f, 0.3f, 0.0f);   /* silence */
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);

    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 4u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NO_ONSET_DETECTED, e.meta.reason_code);
    TEST_ASSERT_EQUAL_UINT32(1u, obs_count(TRUING_ACOUSTIC_PHASE_LISTENING));
    TEST_ASSERT_EQUAL_UINT32(0u, obs_count(TRUING_ACOUSTIC_PHASE_ONSET_DETECTED));
    TEST_ASSERT_EQUAL_UINT32(0u, obs_count(TRUING_ACOUSTIC_PHASE_ANALYZING));

    /* The retry: same spoke, same cycle, so the station is told this is attempt 2 and a fresh
     * window is open. Nothing else reports this - the orchestrator stays in the same state. */
    truing_acoustic_measure(&a, 4u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_UINT32(2u, obs_count(TRUING_ACOUSTIC_PHASE_LISTENING));
    TEST_ASSERT_EQUAL_UINT32(2u, g_obs.ev[1].u.acoustic.attempt);
    /* A different spoke is a new attempt count, not a continuation. */
    truing_acoustic_measure(&a, 5u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_UINT32(1u, g_obs.ev[2].u.acoustic.attempt);
    TEST_ASSERT_EQUAL_UINT8(5u, g_obs.ev[2].u.acoustic.spoke_index);
    src.close(&src);
}

/* Each spoke is struck by the actuator at its own acoustic station and by nothing else, with that
 * station's pulse. spoke 0 is LEFT's and assignment alternates (truing_acoustic_station_for_spoke):
 * firing the other station's solenoid would excite a different spoke under this one's name. */
static void test_each_spoke_fires_only_its_own_stations_actuator(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    /* the seam's identity fields */
    TEST_ASSERT_EQUAL_STRING("pluck_fake", g_pl[0].impl_name);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, g_pl[0].source_impl);
    g_excitation.pulse_ms[1] = 35.0f;   /* the stations may differ; each gets its own */
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_EQUAL_STRING("acoustic_real+pluck(L,R)", a.impl_name);
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);

    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_UINT32(1u, g_pctx[0].fires);
    TEST_ASSERT_EQUAL_UINT32(0u, g_pctx[1].fires);
    TEST_ASSERT_EQUAL_FLOAT(g_excitation.pulse_ms[0], g_pctx[0].last_pulse_ms);
    /* commanded BEFORE the window opens, so the LISTENING frame already knows which fired */
    TEST_ASSERT_EQUAL_UINT8(TRUING_ACOUSTIC_PHASE_LISTENING, g_obs.ev[0].u.acoustic.phase);
    TEST_ASSERT_EQUAL_UINT8(TRUING_STATION_ACOUSTIC_LEFT, g_obs.ev[0].u.acoustic.station);
    TEST_ASSERT_TRUE(g_obs.ev[0].u.acoustic.fired);

    memset(&g_obs, 0, sizeof(g_obs));
    truing_acoustic_measure(&a, 1u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_UINT32(1u, g_pctx[0].fires);   /* never both in one measurement */
    TEST_ASSERT_EQUAL_UINT32(1u, g_pctx[1].fires);
    TEST_ASSERT_EQUAL_FLOAT(35.0f, g_pctx[1].last_pulse_ms);
    TEST_ASSERT_EQUAL_UINT8(TRUING_STATION_ACOUSTIC_RIGHT, g_obs.ev[0].u.acoustic.station);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    src.close(&src);
}

static bool minimal_available(truing_pluck_if_t *self) { (void)self; return true; }
static bool minimal_fire(truing_pluck_if_t *self, float pulse_ms) { (void)self; return pulse_ms > 0.0f; }

/* fire_report is optional per pluck_if.h -- NULL means "not supported". An actuator that
 * predates this seam, or simply never implements it, must still let a measurement complete
 * cleanly: fired stays true, but nothing claims a measured width that was never reported. */
static void test_an_actuator_without_fire_report_still_measures_cleanly(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);

    truing_pluck_if_t minimal;
    memset(&minimal, 0, sizeof(minimal));
    minimal.impl_name = "minimal_no_report";
    minimal.source_impl = TRUING_SOURCE_REAL;
    minimal.available = minimal_available;
    minimal.fire = minimal_fire;
    minimal.fire_report = NULL;   /* the case under test */

    truing_acoustic_actuators_t act;
    act.at[0] = &minimal;
    act.at[1] = &minimal;
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &act, g_scratch, g_scratch_bytes, &detail));

    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    truing_acoustic_capture_view_t v;
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    TEST_ASSERT_TRUE(v.diag.fired);
    TEST_ASSERT_FALSE(v.diag.pulse_measured);       /* no report available -- must stay false */
    TEST_ASSERT_EQUAL_UINT32(0u, v.diag.pulse_us_measured);
    src.close(&src);
}

/* A fire() that fails is a rejected attempt with a reason, before any window opens: no phase frame
 * claims an excitation, nothing is captured, the previous capture stays readable, the other station
 * is untouched, and the next attempt (the orchestrator's re-excitation, SPEC §7.4) fires normally.
 * No hand-pluck fallback exists to take over (plan A10). */
static void test_a_failed_fire_rejects_the_attempt_before_any_capture(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &e);   /* a capture to preserve: LEFT fires */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    truing_acoustic_capture_view_t v0, v;
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v0));

    memset(&g_obs, 0, sizeof(g_obs));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);
    g_pctx[0].fail_next = true;
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_EXCITATION_UNAVAILABLE, e.meta.reason_code);
    TEST_ASSERT_TRUE(isnan(e.tension_n));
    TEST_ASSERT_EQUAL_UINT32(0u, g_obs.n);                       /* no window was ever announced */
    TEST_ASSERT_EQUAL_UINT32(v0.seq, truing_acoustic_real_capture_seq(&a));   /* nothing captured */
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    TEST_ASSERT_EQUAL_UINT32(v0.seq, v.seq);
    TEST_ASSERT_EQUAL_UINT32(v0.attempt, v.attempt);
    TEST_ASSERT_EQUAL_UINT8(TRUING_STATION_ACOUSTIC_LEFT, v.diag.station);
    TEST_ASSERT_EQUAL_UINT32(0u, g_pctx[1].fires);               /* the other station was never touched */

    /* the failure was one-shot: the retry fires and measures */
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_TRUE(g_obs.n > 0u);
    TEST_ASSERT_TRUE(g_obs.ev[0].u.acoustic.fired);
    TEST_ASSERT_EQUAL_UINT32(3u, g_obs.ev[0].u.acoustic.attempt);   /* attempts count calls, not captures */
    src.close(&src);
}

/* A station with no actuator - or one that reports itself unavailable - rejects its own spokes and
 * makes the subsystem not ready, so admission refuses the session. The other station still works:
 * sequential bring-up campaign trials stay possible. */
static void test_a_missing_actuator_rejects_its_station_and_is_not_ready(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    truing_tension_estimate_t e;
    truing_reason_t why = TRUING_REASON_NONE;

    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_TRUE(truing_acoustic_ready(&a, &why));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NONE, why);

    truing_acoustic_actuators_t left_only = g_act;
    left_only.at[1] = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &left_only, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_EQUAL_STRING("acoustic_real+pluck(L)", a.impl_name);
    TEST_ASSERT_FALSE(truing_acoustic_ready(&a, &why));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_EXCITATION_UNAVAILABLE, why);
    truing_acoustic_measure(&a, 1u, &g_wheel, 1u, &e);   /* RIGHT's spoke: nothing to strike it */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_EXCITATION_UNAVAILABLE, e.meta.reason_code);
    TEST_ASSERT_EQUAL_UINT32(0u, truing_acoustic_real_capture_seq(&a));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);   /* LEFT's spoke still measures */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);

    /* wired but reporting itself unavailable is the same as absent */
    g_pctx[0].attached = false;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_FALSE(truing_acoustic_ready(&a, &why));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_EXCITATION_UNAVAILABLE, e.meta.reason_code);

    /* no actuators at all */
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, NULL, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_EQUAL_STRING("acoustic_real", a.impl_name);
    TEST_ASSERT_FALSE(truing_acoustic_ready(&a, &why));
    /* and an invalid excitation profile is refused at init, like an invalid chain */
    g_excitation.pulse_ms[0] = 0.0f;
    TEST_ASSERT_FALSE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_EQUAL_STRING("excitation profile invalid", detail);
    src.close(&src);
}

/* Observation is observation: the estimate must not depend on anyone watching (SPEC §13.3). */
static void test_the_estimate_is_identical_with_and_without_an_observer(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    truing_tension_estimate_t quiet, watched;

    truing_audio_synthetic_init(&src, &sctx, 470.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &quiet);
    src.close(&src);

    memset(&g_obs, 0, sizeof(g_obs));
    truing_audio_synthetic_init(&src, &sctx, 470.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &watched);
    src.close(&src);

    TEST_ASSERT_EQUAL_INT(quiet.meta.status, watched.meta.status);
    TEST_ASSERT_EQUAL_INT(quiet.meta.reason_code, watched.meta.reason_code);
    TEST_ASSERT_EQUAL_FLOAT(quiet.tension_n, watched.tension_n);
    TEST_ASSERT_EQUAL_FLOAT(quiet.frequency.selected_frequency_hz, watched.frequency.selected_frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(3u, g_obs.n);   /* and the watched one really was watched */

    /* Detaching mid-life is safe and silences it again. */
    truing_audio_synthetic_init(&src, &sctx, 470.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);
    truing_acoustic_real_set_observer(&a, NULL, NULL);
    const unsigned before = g_obs.n;
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &watched);
    TEST_ASSERT_EQUAL_UINT32(before, g_obs.n);
    src.close(&src);
}

/* Cancellation must not leave the station being told to pluck into a window that is gone. */
static void test_a_cancelled_measurement_emits_no_phase_at_all(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);
    truing_acoustic_request_cancel(&a);
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CANCELLED, e.meta.reason_code);
    TEST_ASSERT_EQUAL_UINT32(0u, g_obs.n);
    src.close(&src);
}

/* A session boundary must discard latched request state that no measurement consumed. An
 * ABORT at a positioning wait sets cancel_requested with nothing to consume it; without
 * reset_session it answers the next session's first measurement as CANCELLED - no pluck cue,
 * no retries (the demo-image spoke-0 skip). reset_session also drops attempt_valid, so the
 * next session's first pluck is "attempt 1", not a continuation of the last one. This is the
 * acoustic_real half of the orchestrator begin_session() regression in test_orch_workflow. */
static void test_reset_session_discards_state_no_measurement_consumed(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act,
                                               g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_real_set_observer(&a, obs_fn, &g_obs);
    truing_tension_estimate_t e;

    /* session 1: two consecutive calls for spoke 0 / cycle 1 - the second is retry "attempt 2" */
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_UINT32(2u, g_obs.ev[g_obs.n - 1u].u.acoustic.attempt);

    /* the session ends at a wait: an ABORT sets the cancel flag and nothing consumes it */
    truing_acoustic_request_cancel(&a);
    truing_acoustic_reset_session(&a);

    /* session 2's first call for the same spoke/cycle: a real result, not the stale cancel,
     * and numbered attempt 1 rather than a continuation */
    memset(&g_obs, 0, sizeof(g_obs));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_NOT_EQUAL(TRUING_REASON_CANCELLED, e.meta.reason_code);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_EQUAL_UINT32(1u, g_obs.ev[0].u.acoustic.attempt);
    src.close(&src);
}

/* I1: an attempt that returns early must not destroy the evidence of the previous capture.
 * Wiping diag at the top of measure_run() made /debug/capture 404 ~3.7 s before the next
 * capture replaced the samples - the harvest race that loses exactly the attempts a fetch
 * came for. The reset now lives where the samples change, so anything that returns before
 * the capture leaves the last one fully readable (SPEC §12.5: evidence of the last
 * measurement; same rule real_reset_session applies at a session boundary). */
static void assert_same_capture(const truing_acoustic_capture_view_t *v0, const truing_acoustic_capture_view_t *v)
{
    TEST_ASSERT_EQUAL_UINT32(v0->seq, v->seq);
    TEST_ASSERT_EQUAL_UINT32(v0->n_words, v->n_words);
    TEST_ASSERT_EQUAL_PTR(v0->words, v->words);
    TEST_ASSERT_EQUAL_UINT32(v0->diag.n_captured, v->diag.n_captured);
    TEST_ASSERT_EQUAL_UINT32(v0->diag.onsets, v->diag.onsets);
    TEST_ASSERT_EQUAL_FLOAT(v0->diag.f1_hz, v->diag.f1_hz);
    TEST_ASSERT_EQUAL_FLOAT(v0->diag.snr_db, v->diag.snr_db);
    TEST_ASSERT_EQUAL_UINT8(v0->spoke_id, v->spoke_id);
    TEST_ASSERT_EQUAL_UINT32(v0->attempt, v->attempt);
}

static void test_the_capture_view_survives_an_aborted_attempt(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 480.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 6u, &g_wheel, 3u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    truing_acoustic_capture_view_t v, v0;
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v0));

    /* cancel before anything opens: the flag is consumed, nothing is captured, and the
     * previous capture must still be exactly what the dump would have read before */
    truing_acoustic_request_cancel(&a);
    truing_acoustic_measure(&a, 6u, &g_wheel, 3u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CANCELLED, e.meta.reason_code);
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    assert_same_capture(&v0, &v);
    TEST_ASSERT_EQUAL_UINT32(v0.seq, truing_acoustic_real_capture_seq(&a));   /* no capture, no bump */

    /* an excitation that fails also returns before the capture, and must leave it standing too */
    g_pctx[0].fail_next = true;
    truing_acoustic_measure(&a, 6u, &g_wheel, 3u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_EXCITATION_UNAVAILABLE, e.meta.reason_code);
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    assert_same_capture(&v0, &v);

    /* and the measurement after the abort is an ordinary retry: seq bumps. Attempt counting
     * keys on calls, not on captures, so this third call is attempt 4 - and the capture view
     * now says the words it holds are THAT attempt's evidence. */
    truing_acoustic_measure(&a, 6u, &g_wheel, 3u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    TEST_ASSERT_EQUAL_UINT32(v0.seq + 1u, v.seq);
    TEST_ASSERT_EQUAL_UINT32(4u, v.attempt);
    src.close(&src);
}

static void test_the_capture_view_survives_a_calibration_missing_early_return(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 470.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    truing_acoustic_capture_view_t v, v0;
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v0));

    /* the defensive path (SPEC 11.3.1) returns before anything is captured; it must not take
     * the previous capture's evidence with it. profile_ok is admission, so it is poked here
     * exactly the way test_status_rules re-inits with an incomplete profile - but on a ctx
     * that already holds a capture. */
    ctx.profile_ok = false;
    truing_acoustic_measure(&a, 2u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, e.meta.reason_code);
    TEST_ASSERT_TRUE(truing_acoustic_real_last_capture(&a, &v));
    assert_same_capture(&v0, &v);
    src.close(&src);
}

static void test_status_rules_calibration_cancel_overrun_silence_format(void)
{
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_audio_synthetic_init(&src, &sctx, 450.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    truing_tension_estimate_t e;
    /* incomplete profile: CALIBRATION_MISSING, never a nominal L_eff (SPEC 11.3.1) */
    truing_tension_model_profile_t incomplete;
    truing_fixture_tension_model_profile_incomplete(&incomplete);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &incomplete, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, e.meta.reason_code);
    TEST_ASSERT_TRUE(isnan(e.tension_n));
    src.close(&src);
    /* cancel before the capture: CANCELLED, the flag is consumed */
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, &g_act, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_request_cancel(&a);
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CANCELLED, e.meta.reason_code);
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    /* silence: no onset, no number */
    sctx.amplitude = 0.0f;
    sctx.noise_rms = 0.0f;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NO_ONSET_DETECTED, e.meta.reason_code);
    /* noise only: whatever the onset detector makes of it, no tension may come out */
    sctx.noise_rms = 1e-3f;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_TRUE(isnan(e.tension_n));
    /* a tone outside the f1 band: strong peaks exist but none qualifies */
    sctx.amplitude = 0.3f;
    sctx.noise_rms = 1e-4f;
    sctx.f1_hz = 1200.0f;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_FREQ_OUT_OF_RANGE, e.meta.reason_code);
    src.close(&src);
    /* dropped samples: the recorded-buffer source can inject an overrun */
    static int32_t words[4800];
    memset(words, 0, sizeof(words));
    truing_audio_source_if_t rec;
    truing_audio_buffer_ctx_t rctx;
    truing_audio_buffer_init(&rec, &rctx, words, 4800u, NULL);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &rec, &g_act, g_scratch, g_scratch_bytes, &detail));
    rctx.inject_overrun = true;
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CAPTURE_OVERRUN, e.meta.reason_code);
    rec.close(&rec);
    /* wrong sample rate: refused at init, never resampled (SPEC 14.4) */
    truing_audio_format_t f44;
    truing_audio_system_format(&f44);
    f44.sample_rate_hz = 44100u;
    truing_audio_buffer_init(&rec, &rctx, words, 4800u, &f44);
    TEST_ASSERT_FALSE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &rec, &g_act, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_NOT_NULL(strstr(detail, "refused"));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    /* scratch too small */
    truing_audio_buffer_init(&rec, &rctx, words, 4800u, NULL);
    TEST_ASSERT_FALSE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &rec, &g_act, g_scratch, 1024u, &detail));
}

/* ---- the complete workflow: real calculation + real acoustic layers on a synthetic front end -- */
typedef struct {
    uint32_t terminal_events;
    truing_terminal_result_t last_terminal;
} sink_ctx_t;

static bool sink_emit(truing_telemetry_if_t *self, const truing_telemetry_event_t *ev)
{
    sink_ctx_t *c = (sink_ctx_t *)self->ctx;
    if (ev->kind == TRUING_EVT_TERMINAL_RESULT) {
        c->terminal_events++;
        c->last_terminal = ev->u.terminal;
    }
    return true;
}

typedef struct {
    truing_solver_config_t solver;
    truing_machine_profile_t machine;
    truing_artifact_t artifact;
    truing_calc_if_t calc;
    truing_calc_artifact_ctx_t cctx;
    truing_audio_source_if_t src;
    truing_audio_synthetic_ctx_t sctx;
    truing_pluck_if_t pluck[TRUING_ACOUSTIC_STATIONS];
    truing_pluck_fake_ctx_t pctx[TRUING_ACOUSTIC_STATIONS];
    truing_acoustic_actuators_t actuators;
    truing_acoustic_if_t acoustic;
    truing_acoustic_real_ctx_t actx;
    truing_runout_if_t runout;
    truing_runout_manual_ctx_t rctx;
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_telemetry_if_t sink;
    sink_ctx_t sctx_sink;
    truing_auto_operator_t op;
    truing_orchestrator_t orch;
} rig_t;

static rig_t g_rig;

static void model_response(void *user, truing_auto_wheel_model_t *wheel, uint8_t spoke, float turns_rev)
{
    const truing_artifact_t *art = (const truing_artifact_t *)user;
    for (uint8_t k = 0; k < art->n_rim_angles; ++k) {
        wheel->lateral_mm[k] += art->phi_u[k][spoke] * turns_rev;
        wheel->radial_mm[k] += art->phi_v[k][spoke] * turns_rev;
    }
}

static void test_workflow_with_real_acoustic_layers_reaches_converged_geometric_only(void)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof(*r));
    truing_fixture_solver_config(&r->solver, 32u);
    truing_fixture_machine_profile(&r->machine);
    const char *detail = NULL;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_OK, truing_artifact_load(fixture_sym32_artifact_blob, fixture_sym32_artifact_blob_len, &g_wheel,
                                                              &r->solver, &r->artifact, &detail));
    truing_calc_artifact_init(&r->calc, &r->cctx, &r->artifact, &g_wheel);
    /* every spoke "sounds" at 460 Hz: the synthetic front end is the same for all */
    truing_audio_synthetic_init(&r->src, &r->sctx, 460.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    for (unsigned i = 0; i < TRUING_ACOUSTIC_STATIONS; ++i) {
        truing_pluck_fake_init(&r->pluck[i], &r->pctx[i], true);
        r->actuators.at[i] = &r->pluck[i];
    }
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&r->acoustic, &r->actx, g_clock, &g_chain, &g_excitation, &g_profile, &r->src,
                                               &r->actuators, g_scratch, g_scratch_bytes, &detail));
    truing_runout_manual_init(&r->runout, &r->rctx, g_clock);
    truing_navigation_manual_init(&r->nav, &r->nctx, g_clock, 32u, 32u, &r->machine);
    r->sink.impl_name = "sink";
    r->sink.source_impl = TRUING_SOURCE_SYNTHETIC;
    r->sink.emit = sink_emit;
    r->sink.ctx = &r->sctx_sink;
    truing_auto_operator_init(&r->op);
    float d0[TRUING_MAX_SPOKES], u[32], v[32];
    memset(d0, 0, sizeof(d0));
    d0[5] = 0.2f;
    d0[17] = -0.15f;
    for (uint8_t k = 0; k < 32u; ++k) {
        float uu = 0.0f, vv = 0.0f;
        for (uint8_t i = 0; i < 32u; ++i) {
            uu += r->artifact.phi_u[k][i] * d0[i];
            vv += r->artifact.phi_v[k][i] * d0[i];
        }
        u[k] = uu;
        v[k] = vv;
    }
    truing_auto_operator_set_wheel(&r->op, 32u, u, v, 0.0f);
    r->op.response_fn = model_response;
    r->op.user = &r->artifact;
    truing_orch_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.wheel = &g_wheel;
    deps.solver = &r->solver;
    deps.chain = &g_chain;
    deps.tension_model = &g_profile;
    deps.machine = &r->machine;
    deps.acoustic = &r->acoustic;
    deps.runout = &r->runout;
    deps.navigation = &r->nav;
    deps.calc = &r->calc;
    deps.telemetry = &r->sink;
    deps.clock = g_clock;
    deps.firmware_version = "test";
    TEST_ASSERT_TRUE(truing_orch_init(&r->orch, &deps));
    uint32_t steps = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_IDLE, truing_orch_run_auto(&r->orch, &r->op, 50u, &steps));
    truing_intent_t start;
    memset(&start, 0, sizeof(start));
    start.type = TRUING_INTENT_START_TRUING;
    truing_reason_t why = TRUING_REASON_NONE;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&r->orch, &start, &why));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, truing_orch_run_auto(&r->orch, &r->op, 40000u, &steps));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&r->orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, r->sctx_sink.last_terminal);
    TEST_ASSERT_TRUE(r->actx.estimates >= 64u);            /* measure + verify passes */
    TEST_ASSERT_EQUAL_UINT32(r->actx.calls, r->pctx[0].fires + r->pctx[1].fires); /* one excitation per measurement */
    TEST_ASSERT_TRUE(r->pctx[0].fires > 0u && r->pctx[1].fires > 0u);             /* both stations did work */
    TEST_ASSERT_EQUAL_UINT32(0u, r->actx.rejections);
    /* every stored tension is the real layer-4 output on the interim layer-3 selection: suspect */
    const truing_wheel_state_t *ws = &r->orch.wheel_state;
    for (uint8_t i = 0; i < 32u; ++i) {
        const truing_tension_estimate_t *e = truing_wheel_state_spoke(ws, i);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e->meta.status);
        TEST_ASSERT_EQUAL_INT(TRUING_REASON_PROVISIONAL_MODE_ID, e->meta.reason_code);
        TEST_ASSERT_FLOAT_WITHIN(0.5f, 460.0f, e->frequency.selected_frequency_hz);
    }
    truing_cycle_provenance_t prov;
    truing_orch_provenance(&r->orch, &prov);
    TEST_ASSERT_TRUE(prov.contains_non_real_implementations);   /* synthetic front end + manual navigation/runout */
    printf("workflow on real DSP: cycles %u, acoustic calls %lu, estimates %lu\n", (unsigned)snap.cycles_run,
           (unsigned long)r->actx.calls, (unsigned long)r->actx.estimates);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_capture_evidence_replays_to_the_same_measurement);
    RUN_TEST(test_synthetic_pluck_yields_a_provisional_estimate_through_all_four_layers);
    RUN_TEST(test_status_rules_calibration_cancel_overrun_silence_format);
    RUN_TEST(test_phase_events_report_the_measurement_lifecycle_in_order);
    RUN_TEST(test_a_silent_capture_reports_listening_but_never_an_onset);
    RUN_TEST(test_each_spoke_fires_only_its_own_stations_actuator);
    RUN_TEST(test_an_actuator_without_fire_report_still_measures_cleanly);
    RUN_TEST(test_a_failed_fire_rejects_the_attempt_before_any_capture);
    RUN_TEST(test_a_missing_actuator_rejects_its_station_and_is_not_ready);
    RUN_TEST(test_the_estimate_is_identical_with_and_without_an_observer);
    RUN_TEST(test_a_cancelled_measurement_emits_no_phase_at_all);
    RUN_TEST(test_the_capture_view_survives_an_aborted_attempt);
    RUN_TEST(test_the_capture_view_survives_a_calibration_missing_early_return);
    RUN_TEST(test_reset_session_discards_state_no_measurement_consumed);
    RUN_TEST(test_workflow_with_real_acoustic_layers_reaches_converged_geometric_only);
    return UNITY_END();
}
