/* Layer-2/3/4 building blocks against numpy/scipy references computed on the host. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing_dsp/analysis.h"
#include "truing_dsp/envelope.h"
#include "truing_dsp/fft.h"
#include "truing_dsp/onset.h"
#include "truing_dsp/peaks.h"
#include "truing_dsp/spectrum.h"
#include "truing_dsp/tension_model.h"
#include "truing_fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

static void naive_dft(const truing_cpx_t *x, uint32_t n, truing_cpx_t *out, bool inverse)
{
    for (uint32_t k = 0; k < n; ++k) {
        double re = 0.0, im = 0.0;
        for (uint32_t j = 0; j < n; ++j) {
            const double ang = (inverse ? 2.0 : -2.0) * 3.14159265358979323846 * (double)k * (double)j / (double)n;
            re += x[j].re * cos(ang) - x[j].im * sin(ang);
            im += x[j].re * sin(ang) + x[j].im * cos(ang);
        }
        out[k].re = (float)re;
        out[k].im = (float)im;
    }
}

static float lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return ((float)(*s >> 8) / 16777216.0f) - 0.5f;
}

static void test_complex_fft_matches_naive_dft(void)
{
    const uint32_t n = 64u;
    truing_cpx_t tw[32], x[64], ref[64];
    truing_fft_plan_t plan;
    TEST_ASSERT_TRUE(truing_fft_plan_init(&plan, n, tw));
    TEST_ASSERT_FALSE(truing_fft_plan_init(&plan, 48u, tw));
    uint32_t s = 7u;
    for (uint32_t i = 0; i < n; ++i) {
        x[i].re = lcg(&s);
        x[i].im = lcg(&s);
    }
    naive_dft(x, n, ref, false);
    truing_fft_complex(&plan, x, false);
    for (uint32_t i = 0; i < n; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, ref[i].re, x[i].re);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, ref[i].im, x[i].im);
    }
    /* inverse (unscaled) brings it back times n */
    truing_fft_complex(&plan, x, true);
    s = 7u;
    for (uint32_t i = 0; i < n; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, lcg(&s) * (float)n, x[i].re);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, lcg(&s) * (float)n, x[i].im);
    }
}

static void test_real_fft_matches_naive_dft(void)
{
    const uint32_t n = 128u;   /* real length; complex plan of 64 */
    truing_cpx_t tw[32], scratch[64], out[65], full[128], ref[128];
    float x[128];
    truing_fft_plan_t plan;
    TEST_ASSERT_TRUE(truing_fft_plan_init(&plan, n / 2u, tw));
    uint32_t s = 99u;
    for (uint32_t i = 0; i < n; ++i) {
        x[i] = lcg(&s);
        full[i].re = x[i];
        full[i].im = 0.0f;
    }
    naive_dft(full, n, ref, false);
    truing_fft_real(&plan, x, scratch, out);
    for (uint32_t k = 0; k <= n / 2u; ++k) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, ref[k].re, out[k].re);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, ref[k].im, out[k].im);
    }
}

static void test_bluestein_dft_and_hilbert_envelope_match_scipy(void)
{
    static uint8_t block[1 << 16];
    truing_envelope_workspace_t ws;
    TEST_ASSERT_TRUE(truing_envelope_workspace_init(&ws, 64u, block, sizeof(block)));
    const float y7[7] = { 0.5f, -1.0f, 2.0f, 0.25f, -0.75f, 1.5f, 0.0f };
    truing_cpx_t in[7], out[7];
    for (int i = 0; i < 7; ++i) { in[i].re = y7[i]; in[i].im = 0.0f; }
    TEST_ASSERT_TRUE(truing_dsp_dft(&ws, in, 7u, false, out));
    const float dft_re[7] = { 2.5f, -0.4518286f, -2.7426150f, 3.6944436f, 3.6944436f, -2.7426150f, -0.4518286f };
    const float dft_im[7] = { 0.0f, -0.1395162f, 1.9737013f, -0.1501284f, 0.1501284f, -1.9737013f, 0.1395162f };
    for (int i = 0; i < 7; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(2e-5f, dft_re[i], out[i].re);
        TEST_ASSERT_FLOAT_WITHIN(2e-5f, dft_im[i], out[i].im);
    }
    float env[8];
    TEST_ASSERT_TRUE(truing_envelope_analytic(&ws, y7, 7u, env));
    const float h7[7] = { 0.69391211f, 1.12647008f, 2.30063835f, 1.99847770f, 1.40550309f, 1.50241700f, 0.29520886f };
    for (int i = 0; i < 7; ++i) TEST_ASSERT_FLOAT_WITHIN(2e-5f, h7[i], env[i]);
    const float y8[8] = { 0.5f, -1.0f, 2.0f, 0.25f, -0.75f, 1.5f, 0.0f, -0.5f };
    TEST_ASSERT_TRUE(truing_envelope_analytic(&ws, y8, 8u, env));
    const float h8[8] = { 0.66026460f, 1.29808683f, 2.21913847f, 1.72971048f, 1.10113040f, 1.52918543f, 1.33654852f, 0.77073936f };
    for (int i = 0; i < 8; ++i) TEST_ASSERT_FLOAT_WITHIN(2e-5f, h8[i], env[i]);
}

static void test_uniform_smoothing_matches_scipy_nearest(void)
{
    const float x[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    float out[8];
    truing_envelope_smooth(x, 8u, 4u, out);
    const float r4[8] = { 1.25f, 1.75f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.25f };
    for (int i = 0; i < 8; ++i) TEST_ASSERT_FLOAT_WITHIN(1e-6f, r4[i], out[i]);
    truing_envelope_smooth(x, 8u, 5u, out);
    const float r5[8] = { 1.6f, 2.2f, 3.0f, 4.0f, 5.0f, 6.0f, 6.8f, 7.4f };
    for (int i = 0; i < 8; ++i) TEST_ASSERT_FLOAT_WITHIN(1e-6f, r5[i], out[i]);
    truing_envelope_smooth(x, 8u, 1u, out);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, out[2]);
}

static void test_parabolic_interpolation_recovers_a_vertex(void)
{
    /* y = -(k - 5.3)^2 + 2 sampled at integers: vertex at 5.3, value 2 */
    float y[12];
    for (int k = 0; k < 12; ++k) y[k] = -((float)k - 5.3f) * ((float)k - 5.3f) + 2.0f;
    float delta, peak;
    truing_spectrum_parabolic(y, 12u, 5u, &delta, &peak);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.3f, delta);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, peak);
    truing_spectrum_parabolic(y, 12u, 0u, &delta, &peak);   /* edge: no interpolation */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, delta);
    TEST_ASSERT_EQUAL_FLOAT(y[0], peak);
}

static void test_peak_prominence_matches_scipy_find_peaks(void)
{
    /* scipy.signal.find_peaks(x, prominence=0) -> [2, 5, 9, 12, 14, 16], prominences [3, 1, 4, 0.5, 5, 5]
     * (the base scan continues through samples EQUAL to the peak, so 14 and 16 both reach the zeros). */
    const float x[19] = { 0, 1, 3, 1, 2, 2, 2, 1, 0, 4, 0, 1, 1.5f, 1, 5, 4.9f, 5, 3, 0 };
    truing_spectrum_t s;
    s.log_mag_db = (float *)x;
    s.n_bins = 19u;
    s.n_fft = 36u;
    s.sample_rate_hz = 36.0f;    /* bin width 1 Hz: bin index == frequency */
    s.bin_width_hz = 1.0f;
    s.n_samples = 19u;
    truing_peak_t peaks[16];
    bool overflow = false;
    const uint32_t n = truing_peaks_find(&s, 0.0f, 18.0f, 0.0f, 0.0f, peaks, 16u, &overflow);
    TEST_ASSERT_FALSE(overflow);
    const uint32_t ref_idx[6] = { 2u, 5u, 9u, 12u, 14u, 16u };
    const float ref_prom[6] = { 3.0f, 1.0f, 4.0f, 0.5f, 5.0f, 5.0f };
    TEST_ASSERT_EQUAL_UINT32(6u, n);
    for (uint32_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_UINT32(ref_idx[i], peaks[i].bin_index);
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, ref_prom[i], peaks[i].prominence_db);
    }
    /* threshold 1.5 keeps [2, 9, 14, 16] (scipy) */
    const uint32_t m = truing_peaks_find(&s, 0.0f, 18.0f, 1.5f, 0.0f, peaks, 16u, &overflow);
    TEST_ASSERT_EQUAL_UINT32(4u, m);
    TEST_ASSERT_EQUAL_UINT32(2u, peaks[0].bin_index);
    TEST_ASSERT_EQUAL_UINT32(9u, peaks[1].bin_index);
    TEST_ASSERT_EQUAL_UINT32(14u, peaks[2].bin_index);
    TEST_ASSERT_EQUAL_UINT32(16u, peaks[3].bin_index);
    /* band restriction uses the BIN frequency; the depth gate drops peaks far below the strongest
     * REFINED magnitude (14 refines to 5.46, 16 to 5.22; 9 stays 4.0 and 12 at 1.5 fall > 1.2 below) */
    const uint32_t b = truing_peaks_find(&s, 8.0f, 18.0f, 0.0f, 1.2f, peaks, 16u, &overflow);
    TEST_ASSERT_EQUAL_UINT32(2u, b);
    TEST_ASSERT_EQUAL_UINT32(14u, peaks[0].bin_index);
    TEST_ASSERT_EQUAL_UINT32(16u, peaks[1].bin_index);
    /* top-by-prominence, f1 rule, f2 rule */
    const uint32_t all = truing_peaks_find(&s, 0.0f, 18.0f, 0.0f, 0.0f, peaks, 16u, &overflow);
    truing_peak_t top[2];
    TEST_ASSERT_EQUAL_UINT32(2u, truing_peaks_top_by_prominence(peaks, all, 2u, top));
    TEST_ASSERT_EQUAL_UINT32(14u, top[0].bin_index);    /* prominence 5 and 5: tie broken by frequency, reported by frequency */
    TEST_ASSERT_EQUAL_UINT32(16u, top[1].bin_index);
    truing_peak_t f1;
    TEST_ASSERT_TRUE(truing_peaks_identify_f1(peaks, all, 4.5f, 18.0f, &f1));
    TEST_ASSERT_EQUAL_UINT32(5u, f1.bin_index);           /* lowest strong peak in band, not the strongest */
    truing_peak_t f2;
    TEST_ASSERT_TRUE(truing_peaks_identify_f2(peaks, all, f1.freq_hz, 1.7f, 2.0f, &f2));
    TEST_ASSERT_EQUAL_UINT32(9u, f2.bin_index);
    TEST_ASSERT_EQUAL_UINT32(3u, truing_peaks_count_in_band(peaks, all, 8.0f, 15.0f));
}

static void test_onset_detection_fires_once_per_burst(void)
{
    truing_chain_profile_t chain;
    truing_fixture_chain_profile_inmp441(&chain);
    truing_dsp_params_t p;
    TEST_ASSERT_TRUE(truing_dsp_params_from_chain(&chain, &p));
    static float x[48000];
    memset(x, 0, sizeof(x));
    /* a decaying tone starting at 0.3 s, a second at 0.9 s (inside the 2.5 s refractory: ignored) */
    for (int i = 14400; i < 48000; ++i) x[i] = 0.5f * expf(-(float)(i - 14400) / 4800.0f) * sinf(0.06f * (float)i);
    for (int i = 43200; i < 48000; ++i) x[i] += 0.4f * sinf(0.05f * (float)i);
    static float env[512];
    truing_onset_result_t r;
    TEST_ASSERT_TRUE(truing_onset_detect(x, 48000u, &p, env, 512u, &r));
    TEST_ASSERT_EQUAL_UINT32(1u, r.n_onsets);
    TEST_ASSERT_TRUE(r.onset_sample[0] >= 14000u && r.onset_sample[0] <= 14640u);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.15f * r.envelope_max, r.threshold);
    /* silence: no onset, no error */
    memset(x, 0, sizeof(x));
    TEST_ASSERT_TRUE(truing_onset_detect(x, 48000u, &p, env, 512u, &r));
    TEST_ASSERT_EQUAL_UINT32(0u, r.n_onsets);
}

static void test_spectrum_of_a_pure_tone_refines_to_the_true_frequency(void)
{
    const uint32_t n = 4800u;   /* 100 ms */
    static float x[4800];
    for (uint32_t i = 0; i < n; ++i) x[i] = 0.3f * sinf(2.0f * 3.14159265f * 440.25f * (float)i / 48000.0f);
    const uint32_t n_fft = truing_spectrum_n_fft(n, 8.0f);
    TEST_ASSERT_EQUAL_UINT32(65536u, n_fft);
    const size_t bytes = truing_spectrum_workspace_bytes(n_fft);
    void *block = malloc(bytes);
    TEST_ASSERT_NOT_NULL(block);
    truing_spectrum_workspace_t ws;
    TEST_ASSERT_TRUE(truing_spectrum_workspace_init(&ws, n_fft, block, bytes));
    truing_spectrum_t s;
    TEST_ASSERT_TRUE(truing_spectrum_compute(&ws, x, n, 48000.0f, 8.0f, &s));
    uint32_t k;
    TEST_ASSERT_TRUE(truing_spectrum_argmax_in_band(&s, 300.0f, 3000.0f, &k));
    float f, m;
    truing_spectrum_refine_bin(&s, k, &f, &m);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 440.25f, f);
    free(block);
}

static void test_tension_models_closed_forms_and_refusals(void)
{
    truing_tension_model_profile_t p;
    truing_fixture_tension_model_profile_complete(&p);
    const float mu = p.linear_density_kg_per_m.value, L = p.L_eff_m.value, ei = p.bending_stiffness_n_m2.value;
    const float f1 = 423.886f;
    truing_tension_result_t r = truing_tension_model_apply(&p, f1, NAN);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 4.0f * mu * L * L * f1 * f1, r.tension_n);
    p.model_name = TRUING_TENSION_MODEL_STIFFNESS_CORRECTED;
    r = truing_tension_model_apply(&p, f1, NAN);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 4.0f * mu * L * L * f1 * f1 - 9.8696044f * ei / (L * L), r.tension_n);
    TEST_ASSERT_TRUE(r.correction_n > 0.0f);
    /* stiffness correction exceeding the ideal tension at a tiny frequency: refused */
    r = truing_tension_model_apply(&p, 1.0f, NAN);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MODEL_REJECTED, r.reason);
    /* two-mode inversion on a synthetic stiff string: f_n^2 = n^2 A + n^4 B, A = T/(4 mu L^2), B = pi^2 EI/(4 mu L^4) */
    p.model_name = TRUING_TENSION_MODEL_HIGHER_MODE;
    const float T = 900.0f, Ltrue = 0.21f;
    const float A = T / (4.0f * mu * Ltrue * Ltrue), B = 9.8696044f * ei / (4.0f * mu * Ltrue * Ltrue * Ltrue * Ltrue);
    const float g1 = sqrtf(A + B), g2 = sqrtf(4.0f * A + 16.0f * B);
    r = truing_tension_model_apply(&p, g1, g2);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, T, r.tension_n);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, Ltrue, r.l_eff_m);
    r = truing_tension_model_apply(&p, g1, NAN);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_NO_F2_PARTNER, r.reason);
    r = truing_tension_model_apply(&p, g1, 1.95f * g1);   /* sub-harmonic partner: non-physical */
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MODEL_REJECTED, r.reason);
    p.l_eff_bounds_hi = 1.05f;                              /* recovered 0.21 vs nominal 0.1935 -> out of bounds */
    r = truing_tension_model_apply(&p, g1, g2);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MODEL_REJECTED, r.reason);
    /* empirical needs its coefficients; the fixture leaves them unestablished -> CALIBRATION_MISSING */
    p.model_name = TRUING_TENSION_MODEL_EMPIRICAL;
    r = truing_tension_model_apply(&p, f1, NAN);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, r.reason);
    p.empirical_a.value = 0.01f;
    p.empirical_a.provenance = TRUING_PROVENANCE_BENCH_CALIBRATED;
    p.empirical_n.value = 2.0f;
    p.empirical_n.provenance = TRUING_PROVENANCE_BENCH_CALIBRATED;
    r = truing_tension_model_apply(&p, 400.0f, NAN);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1600.0f, r.tension_n);
    /* incomplete profile: never a nominal value */
    truing_fixture_tension_model_profile_incomplete(&p);
    r = truing_tension_model_apply(&p, f1, NAN);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, r.reason);
    TEST_ASSERT_TRUE(isnan(r.tension_n));
}

static void test_chain_profile_dsp_fields_are_validated(void)
{
    truing_chain_profile_t c;
    truing_fixture_chain_profile_inmp441(&c);
    const char *field = NULL;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_chain_profile_check(&c, &field));
    truing_chain_profile_t bad = c;
    bad.zero_pad_factor = 0.5f;
    TEST_ASSERT_NOT_EQUAL(TRUING_CFG_OK, truing_chain_profile_check(&bad, &field));
    bad = c;
    bad.max_peaks = 9u;
    TEST_ASSERT_NOT_EQUAL(TRUING_CFG_OK, truing_chain_profile_check(&bad, &field));
    bad = c;
    bad.f1_band_hi_hz = 3500.0f;   /* outside the search band */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_BAND, truing_chain_profile_check(&bad, &field));
    bad = c;
    bad.capture_ms = 100.0f;       /* shorter than gate + window */
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_OUT_OF_RANGE, truing_chain_profile_check(&bad, &field));
    truing_dsp_params_t p;
    TEST_ASSERT_TRUE(truing_dsp_params_from_chain(&c, &p));
    TEST_ASSERT_EQUAL_FLOAT(48000.0f, p.sample_rate_hz);
    TEST_ASSERT_EQUAL_FLOAT(8.0f, p.zero_pad_factor);
    TEST_ASSERT_EQUAL_UINT8(8u, p.max_peaks);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_complex_fft_matches_naive_dft);
    RUN_TEST(test_real_fft_matches_naive_dft);
    RUN_TEST(test_bluestein_dft_and_hilbert_envelope_match_scipy);
    RUN_TEST(test_uniform_smoothing_matches_scipy_nearest);
    RUN_TEST(test_parabolic_interpolation_recovers_a_vertex);
    RUN_TEST(test_peak_prominence_matches_scipy_find_peaks);
    RUN_TEST(test_onset_detection_fires_once_per_burst);
    RUN_TEST(test_spectrum_of_a_pure_tone_refines_to_the_true_frequency);
    RUN_TEST(test_tension_models_closed_forms_and_refusals);
    RUN_TEST(test_chain_profile_dsp_fields_are_validated);
    return UNITY_END();
}
