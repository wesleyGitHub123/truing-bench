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
static truing_tension_model_profile_t g_profile;
static truing_wheel_class_config_t g_wheel;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static void *g_scratch;
static size_t g_scratch_bytes;

void setUp(void)
{
    truing_fixture_chain_profile_inmp441(&g_chain);
    truing_fixture_tension_model_profile_complete(&g_profile);
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
    truing_pluck_if_t pl;
    truing_pluck_fake_ctx_t pctx;
    truing_pluck_fake_init(&pl, &pctx, true);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &src, &pl, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_SYNTHETIC, a.source_impl);
    truing_tension_estimate_t e;
    truing_acoustic_measure(&a, 3u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_UINT32(1u, pctx.fires);
    TEST_ASSERT_EQUAL_FLOAT(g_chain.excitation_pulse_ms, pctx.last_pulse_ms);
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
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &incomplete, &src, NULL, g_scratch, g_scratch_bytes, &detail));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, e.meta.reason_code);
    TEST_ASSERT_TRUE(isnan(e.tension_n));
    src.close(&src);
    /* cancel before the capture: CANCELLED, the flag is consumed */
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &src, NULL, g_scratch, g_scratch_bytes, &detail));
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
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &rec, NULL, g_scratch, g_scratch_bytes, &detail));
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
    TEST_ASSERT_FALSE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &rec, NULL, g_scratch, g_scratch_bytes, &detail));
    TEST_ASSERT_NOT_NULL(strstr(detail, "refused"));
    truing_acoustic_measure(&a, 0u, &g_wheel, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_UNAVAILABLE, e.meta.status);
    /* scratch too small */
    truing_audio_buffer_init(&rec, &rctx, words, 4800u, NULL);
    TEST_ASSERT_FALSE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &rec, NULL, g_scratch, 1024u, &detail));
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
    truing_pluck_if_t pluck;
    truing_pluck_fake_ctx_t pctx;
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
    truing_pluck_fake_init(&r->pluck, &r->pctx, true);
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&r->acoustic, &r->actx, g_clock, &g_chain, &g_profile, &r->src, &r->pluck,
                                               g_scratch, g_scratch_bytes, &detail));
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
    TEST_ASSERT_EQUAL_UINT32(r->actx.calls, r->pctx.fires); /* one excitation commanded per measurement */
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
    RUN_TEST(test_synthetic_pluck_yields_a_provisional_estimate_through_all_four_layers);
    RUN_TEST(test_status_rules_calibration_cancel_overrun_silence_format);
    RUN_TEST(test_workflow_with_real_acoustic_layers_reaches_converged_geometric_only);
    return UNITY_END();
}
