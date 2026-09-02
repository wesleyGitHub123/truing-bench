/* Recorded-campaign golden fixtures (SPEC 14.2, 14.4): the C port must reproduce the research
 * repository's Python pipeline on identical excerpts within tolerance. The excerpts are int32
 * I2S words copied from the campaign WAVs by acoustic_prep/make_golden.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing_dsp/analysis.h"
#include "truing_dsp/onset.h"
#include "truing_fixtures/acoustic_golden.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"

static truing_chain_profile_t g_chain;
static truing_tension_model_profile_t g_profile;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;

void setUp(void)
{
    truing_fixture_chain_profile_inmp441(&g_chain);
    truing_fixture_tension_model_profile_complete(&g_profile);
    truing_fake_clock_init(&g_fc, &g_clock, 10u);
}
void tearDown(void) {}

static float g_worst_f1 = 0.0f, g_worst_snr = 0.0f, g_worst_mag = 0.0f, g_worst_prom = 0.0f;

static void check_case(const acoustic_golden_case_t *c)
{
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    truing_audio_source_if_t src;
    truing_audio_buffer_ctx_t sctx;
    truing_audio_buffer_init(&src, &sctx, c->pcm, c->n_samples, NULL);
    const size_t bytes = truing_acoustic_real_scratch_bytes(&g_chain);
    void *scratch = malloc(bytes);
    TEST_ASSERT_NOT_NULL(scratch);
    const char *detail = NULL;
    TEST_ASSERT_TRUE_MESSAGE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &src, NULL, scratch, bytes, &detail), detail);
    TEST_ASSERT_EQUAL_INT(TRUING_SOURCE_RECORDED, a.source_impl);   /* provenance follows the front end */

    truing_tension_estimate_t e;
    truing_acoustic_real_analyze_words(&a, c->pcm, c->n_samples, 1u, &e);
    const truing_acoustic_real_diag_t *d = &ctx.diag;
    /* Segmentation and gating identical to the reference */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c->onset_sample, d->onset_sample);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c->window_start_sample, d->window.start_sample);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c->window_n_samples, d->window.n_samples);
    TEST_ASSERT_EQUAL_STRING(c->truncated_by, truing_window_truncation_str(d->window.truncated_by));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c->n_fft, d->n_fft);
    /* Layer 2 / interim 3 outputs */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PROVISIONAL_MODE_ID, e.meta.reason_code);
    TEST_ASSERT_EQUAL_INT(TRUING_MODE_ID_PRESUMED_FUNDAMENTAL, e.frequency.mode_identity);
    TEST_ASSERT_EQUAL_UINT16(TRUING_DSP_SELECTION_RULE_VERSION, e.frequency.selection_rule_version);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, c->f1_hz, e.frequency.selected_frequency_hz);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, c->snr_f1_db, e.frequency.snr_db);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c->n_strong_peaks, d->n_strong_peaks);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c->n_peaks_in_band, d->n_peaks_in_band);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)c->n_candidates, e.frequency.n_candidates);
    for (int i = 0; i < c->n_candidates; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(0.05f, c->candidates[i].freq_hz, e.frequency.candidates[i].frequency_hz);
        TEST_ASSERT_FLOAT_WITHIN(0.2f, c->candidates[i].magnitude_db, e.frequency.candidates[i].magnitude_db);
        TEST_ASSERT_FLOAT_WITHIN(0.2f, c->candidates[i].prominence_db, e.frequency.candidates[i].prominence_db);
        const float dm = fabsf(c->candidates[i].magnitude_db - e.frequency.candidates[i].magnitude_db);
        const float dp = fabsf(c->candidates[i].prominence_db - e.frequency.candidates[i].prominence_db);
        if (dm > g_worst_mag) g_worst_mag = dm;
        if (dp > g_worst_prom) g_worst_prom = dp;
    }
    if (isnan(c->f2_hz)) {
        TEST_ASSERT_TRUE(isnan(d->f2_hz));
    } else {
        TEST_ASSERT_FLOAT_WITHIN(0.1f, c->f2_hz, d->f2_hz);
    }
    const float df1 = fabsf(c->f1_hz - e.frequency.selected_frequency_hz);
    const float dsnr = fabsf(c->snr_f1_db - e.frequency.snr_db);
    if (df1 > g_worst_f1) g_worst_f1 = df1;
    if (dsnr > g_worst_snr) g_worst_snr = dsnr;
    /* Layer 4 on the fixture profile (ideal string) matches the reference closed form */
    TEST_ASSERT_EQUAL_INT(TRUING_TENSION_MODEL_IDEAL_STRING, e.model_name);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, c->m0_tension_n, e.tension_n);
    TEST_ASSERT_TRUE(isnan(e.sigma_n));
    TEST_ASSERT_EQUAL_INT(TRUING_CHECK_OK, truing_tension_estimate_check(&e));
    /* the same excerpt under the stiffness-corrected and two-mode profiles */
    truing_tension_model_profile_t p = g_profile;
    p.model_name = TRUING_TENSION_MODEL_STIFFNESS_CORRECTED;
    ctx.profile = &p;
    truing_acoustic_real_analyze_words(&a, c->pcm, c->n_samples, 1u, &e);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, c->m1_tension_n, e.tension_n);
    p.model_name = TRUING_TENSION_MODEL_HIGHER_MODE;
    truing_acoustic_real_analyze_words(&a, c->pcm, c->n_samples, 1u, &e);
    if (isnan(c->m2_tension_n)) {
        TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
        TEST_ASSERT_TRUE(e.meta.reason_code == TRUING_REASON_NO_F2_PARTNER || e.meta.reason_code == TRUING_REASON_MODEL_REJECTED);
    } else {
        TEST_ASSERT_EQUAL_INT(TRUING_STATUS_SUSPECT, e.meta.status);
        TEST_ASSERT_FLOAT_WITHIN(0.01f * c->m2_tension_n, c->m2_tension_n, e.tension_n);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, c->m2_l_eff_m, ctx.diag.l_eff_m);
    }
    src.close(&src);
    free(scratch);
}

static void test_ts00_e2(void) { check_case(&acoustic_golden_cases[0]); }
static void test_ts03_e2(void) { check_case(&acoustic_golden_cases[1]); }
static void test_d3_e1(void) { check_case(&acoustic_golden_cases[2]); }

static void test_recorded_noise_floor_never_yields_a_tension(void)
{
    const uint32_t n = ACOUSTIC_GOLDEN_NOISE_N_SAMPLES;
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    truing_audio_source_if_t src;
    truing_audio_buffer_ctx_t sctx;
    truing_audio_buffer_init(&src, &sctx, ACOUSTIC_GOLDEN_NOISE_PCM, n, NULL);
    const size_t bytes = truing_acoustic_real_scratch_bytes(&g_chain);
    void *scratch = malloc(bytes);
    const char *detail = NULL;
    TEST_ASSERT_TRUE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_profile, &src, NULL, scratch, bytes, &detail));
    truing_tension_estimate_t e;
    truing_acoustic_real_analyze_words(&a, ACOUSTIC_GOLDEN_NOISE_PCM, n, 1u, &e);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, e.meta.status);
    TEST_ASSERT_TRUE(e.meta.reason_code == TRUING_REASON_NO_ONSET_DETECTED || e.meta.reason_code == TRUING_REASON_LOW_SNR ||
                     e.meta.reason_code == TRUING_REASON_FREQ_OUT_OF_RANGE || e.meta.reason_code == TRUING_REASON_VALUE_OUT_OF_RANGE);
    TEST_ASSERT_TRUE(isnan(e.tension_n));
    printf("noise floor (%.1f dBFS rms): rejected with %s (onsets %lu, strong peaks %lu, snr %.1f dB)\n",
           (double)ACOUSTIC_GOLDEN_NOISE_RMS_DBFS, truing_reason_str(e.meta.reason_code), (unsigned long)ctx.diag.onsets,
           (unsigned long)ctx.diag.n_strong_peaks, (double)ctx.diag.snr_db);
    src.close(&src);
    free(scratch);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ts00_e2);
    RUN_TEST(test_ts03_e2);
    RUN_TEST(test_d3_e1);
    RUN_TEST(test_recorded_noise_floor_never_yields_a_tension);
    printf("golden parity: worst |df1| %.4f Hz, |dSNR| %.3f dB, |dmag| %.3f dB, |dprom| %.3f dB over %u recorded plucks\n",
           (double)g_worst_f1, (double)g_worst_snr, (double)g_worst_mag, (double)g_worst_prom, (unsigned)ACOUSTIC_GOLDEN_N_CASES);
    return UNITY_END();
}
