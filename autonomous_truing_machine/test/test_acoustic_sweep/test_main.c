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
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing/config.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"
#include "truing_proto/json.h"

#define MAX_PATH_LEN   512u
#define MAX_JSON_BYTES 8192u
#define MAX_CAPTURES   128u
#define MAX_NAME_LEN   96u

static truing_chain_profile_t g_base;
static truing_tension_model_profile_t g_profile;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static char g_dir[MAX_PATH_LEN];

void setUp(void)
{
    truing_fixture_chain_profile_inmp441(&g_base);
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
    if (!truing_acoustic_real_init(&a, &ctx, g_clock, chain, &g_profile, &src, NULL, scratch, bytes, &detail)) {
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

/* The grid: field name, and the values to try. Each list is swept with every other field held
 * at the fixture profile. Keep these in step with the plan's Work / A4 list. */
static void sweep_bundle(const char *dir, const char *name)
{
    char path[MAX_PATH_LEN];
    static char json[MAX_JSON_BYTES];
    (void)snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    const size_t jn = read_file(path, json, sizeof(json) - 1u);
    if (jn == 0u) {
        printf("%s,,,METADATA_MISSING,,,,,,,\n", name);
        return;
    }
    json[jn] = '\0';
    truing_json_doc_t doc;
    if (!truing_json_doc_init(&doc, json, jn)) {
        printf("%s,,,METADATA_BAD,,,,,,,\n", name);
        return;
    }
    uint32_t n_words = 0u;
    if (!json_u32(&doc, "n_words", &n_words) || n_words == 0u) {
        printf("%s,,,N_WORDS_MISSING,,,,,,,\n", name);
        return;
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
        printf("%s,,,PCM_SHORT,,,,,,,\n", name);
        free(words);
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

static void test_sweep(void)
{
    if (!find_dir(g_dir, sizeof(g_dir))) {
        TEST_IGNORE_MESSAGE("no bundle dir with an index.txt found "
                            "(set TRUING_SWEEP_DIR; a campaign_recorder run writes one)");
    }
    char path[MAX_PATH_LEN];
    (void)snprintf(path, sizeof(path), "%s/index.txt", g_dir);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    char names[MAX_CAPTURES][MAX_NAME_LEN];
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
    RUN_TEST(test_sweep);
    return UNITY_END();
}
