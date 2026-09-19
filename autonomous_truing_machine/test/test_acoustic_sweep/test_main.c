/* DSP parameter sweep over captured audio (evidence for the acoustic retune, SPEC 9.5).
 *
 * Not a regression suite. It replays every bundle in a directory through the firmware's own
 * acoustic layers 2-4 -- the same truing_acoustic_real_analyze_words() the board and
 * test_acoustic_replay use -- once per parameter setting, and prints one CSV row per
 * (bundle, parameter, value): n_strong_peaks, n_peaks_in_band, f1, f2, snr_db, and whether
 * that setting would have cleared measurement_min_snr_db. Nothing is asserted; the test
 * "passes" if the run completes.
 *
 * Why its own env and not part of `-e native`: this walks ~15 settings across every bundle at
 * a 262144-point FFT, which is tens of seconds. T2's value is being 45 s. Run it deliberately:
 *
 *     TRUING_SWEEP_DIR=test/fixtures/acoustic/captures/_campaign \
 *       pio test -d "$USERPROFILE/truing_ws" -e native_sweep -v
 *
 * DELIBERATELY NO chain_digest check. test_acoustic_replay refuses a bundle whose digest does
 * not match the build because replaying it would misreport the original measurement. Here the
 * whole point is to replay the same samples under *different* constants, so the digest is
 * irrelevant and the bundles never need re-capturing to be swept.
 *
 * One axis at a time: every setting varies a single field and holds the rest at the INMP441
 * fixture profile's value, so a row's effect is attributable. The baseline row (the profile
 * as-is) is printed once at the top for reference.
 *
 * TRUING_SWEEP_MODE=lines switches to a different question: not "would this setting clear" but
 * "what strong peaks does the firmware's own detector find in this capture, before any clear or
 * reject gate". One CSV row per strong peak. That is B3.0's evidence for a coherent line
 * (docs/SOLENOID_CAMPAIGN.md, B3.0 pre-registration): a control the firmware rejected at the onset
 * stage has no spectrum at all, so the mode re-runs the onset with the absolute floor disabled --
 * the only override -- to expose what the detector would have seen.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing/config.h"
#include "truing_dsp/analysis.h"
#include "truing_dsp/onset.h"
#include "truing_dsp/params.h"
#include "truing_dsp/peaks.h"
#include "truing_dsp/spectrum.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"
#include "truing_proto/json.h"

#define MAX_PATH_LEN   512u
#define MAX_JSON_BYTES 8192u
#define MAX_CAPTURES   512u
#define MAX_NAME_LEN   96u

static truing_chain_profile_t g_base;
static truing_excitation_profile_t g_excitation;
static truing_tension_model_profile_t g_profile;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static char g_dir[MAX_PATH_LEN];

void setUp(void)
{
    truing_fixture_chain_profile_inmp441(&g_base);
    truing_fixture_excitation_profile(&g_excitation);
    truing_fixture_tension_model_profile_complete(&g_profile);
    truing_fake_clock_init(&g_fc, &g_clock, 10u);
}
void tearDown(void) {}

static const char *const k_candidates[] = {
    "test/fixtures/acoustic/captures/_campaign",
    "../test/fixtures/acoustic/captures/_campaign",
    "../../test/fixtures/acoustic/captures/_campaign",
    "../../../test/fixtures/acoustic/captures/_campaign",
    "test/fixtures/acoustic/captures",
    "../test/fixtures/acoustic/captures",
    "../../test/fixtures/acoustic/captures",
    "../../../test/fixtures/acoustic/captures",
};

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static bool find_dir(char *out, size_t cap)
{
    char probe[MAX_PATH_LEN];
    const char *env = getenv("TRUING_SWEEP_DIR");
    if (env != NULL && env[0] != '\0') {
        (void)snprintf(probe, sizeof(probe), "%s/index.txt", env);
        if (file_exists(probe)) {
            (void)snprintf(out, cap, "%s", env);
            return true;
        }
        printf("TRUING_SWEEP_DIR=%s has no index.txt\n", env);
        return false;
    }
    for (size_t i = 0; i < sizeof(k_candidates) / sizeof(k_candidates[0]); ++i) {
        (void)snprintf(probe, sizeof(probe), "%s/index.txt", k_candidates[i]);
        if (file_exists(probe)) {
            (void)snprintf(out, cap, "%s", k_candidates[i]);
            return true;
        }
    }
    return false;
}

static size_t read_file(const char *path, void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0u;
    }
    const size_t n = fread(buf, 1u, cap, f);
    fclose(f);
    return n;
}

static bool json_u32(const truing_json_doc_t *d, const char *key, uint32_t *out)
{
    truing_json_value_t v;
    if (truing_json_get(d, key, &v) != TRUING_JSON_NUMBER) {
        return false;
    }
    *out = (uint32_t)(v.number + 0.5);
    return true;
}

static bool json_str(const truing_json_doc_t *d, const char *key, char *out, size_t cap)
{
    truing_json_value_t v;
    if (truing_json_get(d, key, &v) != TRUING_JSON_STRING) {
        return false;
    }
    return truing_json_copy_str(&v, out, cap);
}

/* CSV field for a float that may be NaN. MinGW's printf writes "1.#QO" for NaN, which no
 * spreadsheet or reader will take; an empty field is what "not found" means. */
static void csv_f(char *out, size_t cap, float v)
{
    if (isfinite(v)) {
        (void)snprintf(out, cap, "%.3f", (double)v);
    } else if (cap > 0u) {
        out[0] = '\0';
    }
}

/* One analysis pass on one buffer under one chain profile. */
static void run_one(const char *name, const char *axis, double value, const truing_chain_profile_t *chain,
                    const int32_t *words, uint32_t n_words)
{
    /* A setting the config validator rejects is a finding, not an error: it says the field
     * cannot go there without moving another (gate_start_ms + window_ms must fit capture_ms). */
    const char *field = NULL;
    if (truing_chain_profile_check(chain, &field) != TRUING_CFG_OK) {
        printf("%s,%s,%g,OUT_OF_RANGE,%s,,,,,,\n", name, axis, value, field != NULL ? field : "?");
        return;
    }
    truing_audio_source_if_t src;
    truing_audio_buffer_ctx_t sctx;
    truing_audio_buffer_init(&src, &sctx, (int32_t *)words, n_words, NULL);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const size_t bytes = truing_acoustic_real_scratch_bytes(chain);
    void *scratch = malloc(bytes);
    TEST_ASSERT_NOT_NULL(scratch);
    const char *detail = NULL;
    if (!truing_acoustic_real_init(&a, &ctx, g_clock, chain, &g_excitation, &g_profile, &src, NULL, scratch, bytes, &detail)) {
        printf("%s,%s,%g,INIT_REFUSED,%s,,,,,,\n", name, axis, value, detail);
        free(scratch);
        return;
    }
    truing_tension_estimate_t e;
    truing_acoustic_real_analyze_words(&a, words, n_words, 1u, &e);
    const truing_acoustic_real_diag_t *d = &ctx.diag;
    const int clears = (isfinite(d->snr_db) && d->snr_db >= chain->measurement_min_snr_db) ? 1 : 0;
    char f1[24], f2[24], snr[24];
    csv_f(f1, sizeof(f1), d->f1_hz);
    csv_f(f2, sizeof(f2), d->f2_hz);
    csv_f(snr, sizeof(snr), d->snr_db);
    printf("%s,%s,%g,%s,%s,%u,%u,%s,%s,%s,%d\n",
           name, axis, value,
           truing_status_str(e.meta.status), truing_reason_str(e.meta.reason_code),
           (unsigned)d->n_strong_peaks, (unsigned)d->n_peaks_in_band, f1, f2, snr, clears);
    free(scratch);
}

/* One bundle's samples, or NULL with *err naming why (the caller prints its own CSV row). */
static int32_t *load_words(const char *dir, const char *name, uint32_t *n_words_out, const char **err)
{
    char path[MAX_PATH_LEN];
    static char json[MAX_JSON_BYTES];
    (void)snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    const size_t jn = read_file(path, json, sizeof(json) - 1u);
    if (jn == 0u) {
        *err = "METADATA_MISSING";
        return NULL;
    }
    json[jn] = '\0';
    truing_json_doc_t doc;
    if (!truing_json_doc_init(&doc, json, jn)) {
        *err = "METADATA_BAD";
        return NULL;
    }
    uint32_t n_words = 0u;
    if (!json_u32(&doc, "n_words", &n_words) || n_words == 0u) {
        *err = "N_WORDS_MISSING";
        return NULL;
    }
    char pcm_name[MAX_NAME_LEN] = { 0 };
    if (!json_str(&doc, "pcm", pcm_name, sizeof(pcm_name))) {
        (void)snprintf(pcm_name, sizeof(pcm_name), "%s.pcm", name);
    }
    (void)snprintf(path, sizeof(path), "%s/%s", dir, pcm_name);
    int32_t *words = (int32_t *)malloc((size_t)n_words * sizeof(int32_t));
    TEST_ASSERT_NOT_NULL(words);
    const size_t got = read_file(path, words, (size_t)n_words * sizeof(int32_t));
    if (got != (size_t)n_words * sizeof(int32_t)) {
        *err = "PCM_SHORT";
        free(words);
        return NULL;
    }
    *n_words_out = n_words;
    return words;
}

/* The grid: field name, and the values to try. Each list is swept with every other field held
 * at the fixture profile. Keep these in step with the plan's Work / A4 list. */
static void sweep_bundle(const char *dir, const char *name)
{
    uint32_t n_words = 0u;
    const char *err = NULL;
    int32_t *words = load_words(dir, name, &n_words, &err);
    if (words == NULL) {
        printf("%s,,,%s,,,,,,,\n", name, err);
        return;
    }

    /* baseline */
    run_one(name, "baseline", 0, &g_base, words, n_words);

    const float gate[]  = { 100.0f, 200.0f, 300.0f, 500.0f, 800.0f };
    const float win[]   = { 250.0f, 750.0f };
    const float prom[]  = { 9.0f, 12.0f };
    const float depth[] = { 20.0f };
    const float bandhi[] = { 1200.0f, 2000.0f };
    const float minsnr[] = { 6.0f, 9.0f };

    truing_chain_profile_t c;
    for (size_t i = 0; i < sizeof(gate) / sizeof(gate[0]); ++i) {
        c = g_base; c.gate_start_ms = gate[i];
        run_one(name, "gate_start_ms", gate[i], &c, words, n_words);
    }
    for (size_t i = 0; i < sizeof(win) / sizeof(win[0]); ++i) {
        c = g_base; c.window_ms = win[i];
        run_one(name, "window_ms", win[i], &c, words, n_words);
    }
    for (size_t i = 0; i < sizeof(prom) / sizeof(prom[0]); ++i) {
        c = g_base; c.prominence_db = prom[i];
        run_one(name, "prominence_db", prom[i], &c, words, n_words);
    }
    for (size_t i = 0; i < sizeof(depth) / sizeof(depth[0]); ++i) {
        c = g_base; c.max_peak_depth_db = depth[i];
        run_one(name, "max_peak_depth_db", depth[i], &c, words, n_words);
    }
    for (size_t i = 0; i < sizeof(bandhi) / sizeof(bandhi[0]); ++i) {
        c = g_base; c.search_band_hi_hz = bandhi[i];
        run_one(name, "search_band_hi_hz", bandhi[i], &c, words, n_words);
    }
    for (size_t i = 0; i < sizeof(minsnr) / sizeof(minsnr[0]); ++i) {
        c = g_base; c.measurement_min_snr_db = minsnr[i];
        run_one(name, "measurement_min_snr_db", minsnr[i], &c, words, n_words);
    }

    free(words);
}

/* ---- lines mode -------------------------------------------------------------------------------
 *
 * Every strong peak the firmware detector reports for a capture, before any clear or reject gate.
 * "Strong" is the detector's own definition: truing_peaks_find() with the chain profile's
 * prominence_db and max_peak_depth_db over its search band -- the very list truing_dsp_analyze_window
 * then selects f1 from. Nothing here re-implements the detector; the peak count from the direct
 * call is asserted equal to the one analyze_window reports for the same window, so the two cannot
 * drift apart unnoticed.
 *
 * Onset: the firmware anchors the window on the first onset, and a capture with none (a quiet
 * control) is rejected before any spectrum exists. So the onset is found twice -- once with the
 * profile as it is (onset_with_floor: what the firmware would do) and once with the absolute RMS
 * floor disabled, which is the ONLY override, and is what places the window. In a control that is
 * the loudest event: the plunger impact for an air shot, the loudest noise frame for a no-fire. The
 * window then runs the profile's full window_ms with no next-onset truncation, so every control is
 * analysed at the same resolution. */
static uint32_t onset_frames_for(const truing_dsp_params_t *p, uint32_t n)
{
    uint32_t frame = (uint32_t)(p->onset_frame_ms * 1e-3f * p->sample_rate_hz + 0.5f);
    uint32_t hop = (uint32_t)(p->onset_hop_ms * 1e-3f * p->sample_rate_hz + 0.5f);
    if (frame == 0u) frame = 1u;
    if (hop == 0u) hop = 1u;
    return n >= frame ? 1u + (n - frame) / hop : 0u;
}

static void lines_bundle(const char *dir, const char *name)
{
    uint32_t n_words = 0u;
    const char *err = NULL;
    int32_t *words = load_words(dir, name, &n_words, &err);
    if (words == NULL) {
        printf("%s,%s,,,,,,,,,,,,,\n", name, err);
        return;
    }
    truing_dsp_params_t p;
    TEST_ASSERT_TRUE(truing_dsp_params_from_chain(&g_base, &p));
    float *x = (float *)malloc((size_t)n_words * sizeof(float));
    TEST_ASSERT_NOT_NULL(x);
    for (uint32_t i = 0u; i < n_words; ++i) {
        x[i] = truing_audio_word_to_float(words[i]);
    }
    free(words);
    const uint32_t env_cap = onset_frames_for(&p, n_words) + 1u;
    float *env = (float *)malloc((size_t)env_cap * sizeof(float));
    TEST_ASSERT_NOT_NULL(env);

    truing_onset_result_t on;
    const bool with_floor = truing_onset_detect(x, n_words, &p, env, env_cap, &on) && on.n_onsets > 0u;
    truing_dsp_params_t forced = p;
    forced.onset_threshold_abs = 0.0f;   /* <= 0 disables the absolute floor: the one override */
    truing_onset_result_t on2;
    if (!truing_onset_detect(x, n_words, &forced, env, env_cap, &on2) || on2.n_onsets == 0u) {
        printf("%s,%d,,,,,,,,,,,,NO_ONSET_EVEN_FORCED\n", name, with_floor ? 1 : 0);
        free(env);
        free(x);
        return;
    }
    const uint32_t max_window = (uint32_t)(g_base.window_ms * 1e-3f * (float)g_base.sample_rate_hz + 0.5f) + 1u;
    const size_t ws_bytes = truing_dsp_workspace_bytes(max_window, g_base.zero_pad_factor);
    void *block = malloc(ws_bytes);
    TEST_ASSERT_NOT_NULL(block);
    truing_dsp_workspace_t ws;
    TEST_ASSERT_TRUE(truing_dsp_workspace_init(&ws, max_window, g_base.zero_pad_factor, block, ws_bytes));

    truing_dsp_window_t w;
    if (!truing_dsp_select_window(&ws, x, n_words, (int64_t)on2.onset_sample[0], -1, &p, &w)) {
        printf("%s,%d,%u,,,,,,,,,,,NO_WINDOW\n", name, with_floor ? 1 : 0, (unsigned)on2.onset_sample[0]);
    } else {
        truing_spectrum_t spec;
        TEST_ASSERT_TRUE(truing_spectrum_compute(&ws.spectrum, x + w.start_sample, w.n_samples, p.sample_rate_hz,
                                                 p.zero_pad_factor, &spec));
        truing_peak_t strong[TRUING_DSP_MAX_STRONG_PEAKS];
        bool overflow = false;
        const uint32_t n_strong = truing_peaks_find(&spec, p.search_band_lo_hz, p.search_band_hi_hz, p.prominence_db,
                                                    p.max_peak_depth_db, strong, TRUING_DSP_MAX_STRONG_PEAKS, &overflow);
        /* Each strong peak's SNR, by the firmware's own statistic (truing_peaks_snr_db: peak magnitude
         * against the median of the annulus snr_noise_offset_lo..hi Hz away from it -- the quantity
         * measurement_min_snr_db gates f1 on). Prominence is NOT a line-strength measure on a
         * zero-padded noise spectrum: null depth dominates it, and pure noise shows 6-45 dB. */
        float snr[TRUING_DSP_MAX_STRONG_PEAKS];
        for (uint32_t i = 0u; i < n_strong; ++i) {
            snr[i] = truing_peaks_snr_db(&spec, strong[i].freq_hz, strong[i].magnitude_db, p.snr_noise_offset_lo_hz,
                                         p.snr_noise_offset_hi_hz, ws.spectrum.windowed, spec.n_fft);
        }
        /* parity with the firmware's own analysis of the same window (analyze_window reuses the
         * spectrum workspace, so it runs after the direct call has finished with `spec`) */
        truing_dsp_event_t ev;
        TEST_ASSERT_TRUE(truing_dsp_analyze_window(&ws, x, &w, &p, &ev));
        TEST_ASSERT_EQUAL_UINT32(ev.n_strong_peaks, n_strong);
        if (n_strong == 0u) {
            printf("%s,%d,%u,%u,%u,%u,0,%d,,,,,,\n", name, with_floor ? 1 : 0, (unsigned)on2.onset_sample[0],
                   (unsigned)w.start_sample, (unsigned)w.n_samples, (unsigned)spec.n_fft, overflow ? 1 : 0);
        }
        for (uint32_t i = 0u; i < n_strong; ++i) {
            const int in_f1 = strong[i].freq_hz >= p.f1_band_lo_hz && strong[i].freq_hz <= p.f1_band_hi_hz;
            char snr_s[24];
            csv_f(snr_s, sizeof(snr_s), snr[i]);
            printf("%s,%d,%u,%u,%u,%u,%u,%d,%u,%.3f,%.2f,%.2f,%s,%d\n", name, with_floor ? 1 : 0,
                   (unsigned)on2.onset_sample[0], (unsigned)w.start_sample, (unsigned)w.n_samples, (unsigned)spec.n_fft,
                   (unsigned)n_strong, overflow ? 1 : 0, (unsigned)i, (double)strong[i].freq_hz,
                   (double)strong[i].prominence_db, (double)strong[i].magnitude_db, snr_s, in_f1);
        }
    }
    free(block);
    free(env);
    free(x);
}

/* The bundle names in the index, in order. Returns how many. */
static unsigned read_index(char names[][MAX_NAME_LEN])
{
    if (!find_dir(g_dir, sizeof(g_dir))) {
        TEST_IGNORE_MESSAGE("no bundle dir with an index.txt found "
                            "(set TRUING_SWEEP_DIR; a campaign_recorder run writes one)");
    }
    char path[MAX_PATH_LEN];
    (void)snprintf(path, sizeof(path), "%s/index.txt", g_dir);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    unsigned n = 0u;
    char line[MAX_NAME_LEN];
    while (fgets(line, sizeof(line), f) != NULL && n < MAX_CAPTURES) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (*p == '\0' || *p == '#') {
            continue;
        }
        (void)snprintf(names[n++], MAX_NAME_LEN, "%s", p);
    }
    fclose(f);
    return n;
}

static void test_lines(void)
{
    static char names[MAX_CAPTURES][MAX_NAME_LEN];
    const unsigned n = read_index(names);
    /* The verdict tool reads these from here rather than assuming them: a line is a strong peak in the f1
     * band whose SNR reaches min_snr, the chain profile's own measurement_min_snr_db. */
    /* The analysis runs under the fixture chain profile; a capture was taken under the board's. The digest
     * lets the verdict tool refuse a set whose captures were taken under different constants. */
    uint8_t digest[TRUING_SHA256_DIGEST_BYTES];
    truing_chain_profile_digest(&g_base, digest);
    char hex[2u * TRUING_SHA256_DIGEST_BYTES + 1u];
    for (unsigned i = 0u; i < TRUING_SHA256_DIGEST_BYTES; ++i) {
        (void)snprintf(&hex[2u * i], 3u, "%02x", digest[i]);
    }
    printf("\nlines dir: %s   bundles: %u   f1_band %.0f-%.0f Hz, prominence %.1f dB, depth %.1f dB, min_snr %.1f dB, "
           "chain_digest %s\n",
           g_dir, n, (double)g_base.f1_band_lo_hz, (double)g_base.f1_band_hi_hz, (double)g_base.prominence_db,
           (double)g_base.max_peak_depth_db, (double)g_base.measurement_min_snr_db, hex);
    printf("bundle,onset_with_floor,onset_forced_sample,window_start,window_n,n_fft,n_strong,overflow,idx,freq_hz,"
           "prominence_db,magnitude_db,snr_db,in_f1_band\n");
    for (unsigned i = 0u; i < n; ++i) {
        lines_bundle(g_dir, names[i]);
    }
    printf("lines complete: %u bundle(s)\n", n);
}

static void test_sweep(void)
{
    static char names[MAX_CAPTURES][MAX_NAME_LEN];
    const unsigned n = read_index(names);
    printf("\nsweep dir: %s   bundles: %u\n", g_dir, n);
    printf("bundle,axis,value,status,reason,n_strong_peaks,n_peaks_in_band,f1_hz,f2_hz,snr_db,clears_min_snr\n");
    for (unsigned i = 0u; i < n; ++i) {
        sweep_bundle(g_dir, names[i]);
    }
    printf("sweep complete: %u bundle(s)\n", n);
}

int main(void)
{
    UNITY_BEGIN();
    const char *mode = getenv("TRUING_SWEEP_MODE");
    if (mode != NULL && strcmp(mode, "lines") == 0) {
        RUN_TEST(test_lines);
    } else {
        RUN_TEST(test_sweep);
    }
    return UNITY_END();
}
