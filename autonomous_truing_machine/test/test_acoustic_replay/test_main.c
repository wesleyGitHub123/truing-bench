/* Replay of captured acoustic evidence (SPEC §12.5, §14.2, §14.4).
 *
 * A pluck that behaves badly on the bench is, on its own, unrepeatable: it happened once, to
 * one spoke, on one wheel, and the next attempt is a different pluck. The only way to work on
 * it is to keep the samples. This suite is the host end of that loop — it takes the capture
 * bundles under test/fixtures/acoustic/captures and pushes each one back through the same
 * acoustic layers 2-4 that produced the original result, with no hardware anywhere.
 *
 * A bundle is two files: `<name>.pcm`, the raw int32 words the front end delivered, and
 * `<name>.json`, a flat object recording where they came from and what they meant. The JSON
 * carries the digest of the acoustic chain configuration in force at capture time, and a
 * bundle whose digest does not match this build's is REFUSED rather than replayed: the same
 * samples under different DSP constants give a different answer, and silently reporting that
 * answer would be worse than not replaying at all.
 *
 * `expect_*` keys, where present, are the regression. `expect_origin` says what they are
 * evidence of:
 *   board   the numbers the board itself produced from these words. Agreement here is
 *           host/target parity on identical bytes - the strongest thing this suite can say.
 *   golden  the research pipeline's reference values for a recorded excerpt.
 *   host    a previously accepted host result. Change detection only; it proves the pipeline
 *           still does what it did, not that what it does is right.
 *
 * A bundle with no expectations is not a failure. It is replayed, its diagnostics are printed,
 * and `<name>.observed.json` is written next to it so the numbers can be inspected and, if
 * they are the ones to hold onto, promoted with tools/capture_accept.py.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "truing/config.h"
#include "truing_dsp/analysis.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"
#include "truing_proto/json.h"

#define MAX_PATH_LEN   512u
#define MAX_JSON_BYTES 8192u
#define MAX_CAPTURES   64u
#define MAX_NAME_LEN   96u

static truing_chain_profile_t g_chain;
static truing_excitation_profile_t g_excitation;
static truing_tension_model_profile_t g_profile;
static truing_fake_clock_t g_fc;
static truing_clock_if_t g_clock;
static char g_dir[MAX_PATH_LEN];

void setUp(void)
{
    truing_fixture_chain_profile_inmp441(&g_chain);
    truing_fixture_excitation_profile(&g_excitation);
    truing_fixture_tension_model_profile_complete(&g_profile);
    truing_fake_clock_init(&g_fc, &g_clock, 10u);
}
void tearDown(void) {}

/* ---- locating the fixtures ---------------------------------------------------------------
 *
 * The test binary's working directory is a property of the runner, not of this repository, so
 * the directory is searched for rather than assumed. TRUING_CAPTURES_DIR overrides everything,
 * which is how a one-off bundle somewhere else gets replayed without being checked in. */
static const char *const k_candidates[] = {
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

static bool find_captures_dir(char *out, size_t cap)
{
    char probe[MAX_PATH_LEN];
    const char *env = getenv("TRUING_CAPTURES_DIR");
    if (env != NULL && env[0] != '\0') {
        (void)snprintf(probe, sizeof(probe), "%s/index.txt", env);
        if (file_exists(probe)) {
            (void)snprintf(out, cap, "%s", env);
            return true;
        }
        printf("TRUING_CAPTURES_DIR=%s has no index.txt\n", env);
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

/* ---- bundle loading ---------------------------------------------------------------------- */

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

static bool json_f32(const truing_json_doc_t *d, const char *key, float *out)
{
    truing_json_value_t v;
    if (truing_json_get(d, key, &v) != TRUING_JSON_NUMBER) {
        return false;
    }
    *out = (float)v.number;
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

static void hex_of(const uint8_t *bytes, size_t n, char *out)
{
    static const char *k = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2u * i] = k[(bytes[i] >> 4) & 0xfu];
        out[2u * i + 1u] = k[bytes[i] & 0xfu];
    }
    out[2u * n] = '\0';
}

/* Relative tolerance, with an absolute floor so a value near zero does not demand exactness.
 * Board and host run the same code on the same bytes but not the same libm or FPU, so a few
 * parts in ten thousand is agreement; a different peak is not. */
static float tol_for(float expected)
{
    const float t = fabsf(expected) * 1e-3f;
    return t < 1e-4f ? 1e-4f : t;
}

/* NaN has no JSON form, and printf spells it differently on every platform - MinGW writes
 * `1.#QNAN0`, which is not a number any JSON reader will take. A rejected measurement has no
 * frequency and no tension, and `null` is what that means. */
static void observed_f32(FILE *f, const char *key, float v, const char *tail)
{
    if (isfinite(v)) {
        fprintf(f, "  \"%s\": %.6f%s\n", key, (double)v, tail);
    } else {
        fprintf(f, "  \"%s\": null%s\n", key, tail);
    }
}

static void write_observed(const char *dir, const char *name, const truing_acoustic_real_diag_t *d,
                           const truing_tension_estimate_t *e)
{
    char path[MAX_PATH_LEN];
    (void)snprintf(path, sizeof(path), "%s/%s.observed.json", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("  (could not write %s)\n", path);
        return;
    }
    /* Written with the keys the bundle itself uses, so promoting a result is a copy and not a
     * translation. tools/capture_accept.py does exactly that. */
    fprintf(f, "{\n");
    fprintf(f, "  \"expect_origin\": \"host\",\n");
    fprintf(f, "  \"expect_status\": \"%s\",\n", truing_status_str(e->meta.status));
    fprintf(f, "  \"expect_reason\": \"%s\",\n", truing_reason_str(e->meta.reason_code));
    fprintf(f, "  \"expect_onset_sample\": %u,\n", (unsigned)d->onset_sample);
    fprintf(f, "  \"expect_onsets\": %u,\n", (unsigned)d->onsets);
    fprintf(f, "  \"expect_window_start_sample\": %u,\n", (unsigned)d->window.start_sample);
    fprintf(f, "  \"expect_window_n_samples\": %u,\n", (unsigned)d->window.n_samples);
    fprintf(f, "  \"expect_window_truncated_by\": \"%s\",\n", truing_window_truncation_str(d->window.truncated_by));
    fprintf(f, "  \"expect_n_fft\": %u,\n", (unsigned)d->n_fft);
    fprintf(f, "  \"expect_n_strong_peaks\": %u,\n", (unsigned)d->n_strong_peaks);
    fprintf(f, "  \"expect_n_peaks_in_band\": %u,\n", (unsigned)d->n_peaks_in_band);
    observed_f32(f, "expect_f1_hz", d->f1_hz, ",");
    observed_f32(f, "expect_snr_db", d->snr_db, ",");
    observed_f32(f, "expect_tension_n", e->tension_n, "");
    fprintf(f, "}\n");
    fclose(f);
}

/* Replays one bundle. Returns false only for a bundle that cannot be replayed at all; a
 * mismatch against recorded expectations fails through unity directly. */
static bool replay_one(const char *dir, const char *name, unsigned *checked)
{
    char path[MAX_PATH_LEN];
    static char json[MAX_JSON_BYTES];
    char msg[256];

    (void)snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    const size_t jn = read_file(path, json, sizeof(json) - 1u);
    if (jn == 0u) {
        (void)snprintf(msg, sizeof(msg), "%s: metadata missing or unreadable", path);
        TEST_FAIL_MESSAGE(msg);
    }
    json[jn] = '\0';
    truing_json_doc_t doc;
    if (!truing_json_doc_init(&doc, json, jn)) {
        (void)snprintf(msg, sizeof(msg), "%s: not a valid flat JSON object", path);
        TEST_FAIL_MESSAGE(msg);
    }

    char schema[64] = { 0 };
    /* /2 adds excitation provenance (station, fired, pulse_ms, excitation_digest). Replay does not
     * gate on the excitation digest: layers 2-4 never read it, so the same words replay the same
     * way whatever struck the spoke. It stays in the bundle as a record of what was captured. */
    if (!json_str(&doc, "schema", schema, sizeof(schema)) ||
        (strcmp(schema, "truing.acoustic.capture/2") != 0 && strcmp(schema, "truing.acoustic.capture/1") != 0)) {
        (void)snprintf(msg, sizeof(msg), "%s: schema is '%s', expected truing.acoustic.capture/2 or /1", name, schema);
        TEST_FAIL_MESSAGE(msg);
    }

    /* The capture is only reproducible under the configuration that produced it (SPEC §9.5). */
    uint8_t digest[TRUING_SHA256_DIGEST_BYTES];
    char here[2u * TRUING_SHA256_DIGEST_BYTES + 1u], recorded[2u * TRUING_SHA256_DIGEST_BYTES + 1u] = { 0 };
    truing_chain_profile_digest(&g_chain, digest);
    hex_of(digest, sizeof(digest), here);
    if (!json_str(&doc, "chain_digest", recorded, sizeof(recorded))) {
        (void)snprintf(msg, sizeof(msg), "%s: no chain_digest; cannot know what it was captured under", name);
        TEST_FAIL_MESSAGE(msg);
    }
    if (strcmp(here, recorded) != 0) {
        (void)snprintf(msg, sizeof(msg),
                       "%s: captured under a different acoustic chain configuration (%.16s... vs %.16s... here) - "
                       "replaying it would answer a question nobody asked",
                       name, recorded, here);
        TEST_FAIL_MESSAGE(msg);
    }

    uint32_t n_words = 0u, rate = 0u;
    if (!json_u32(&doc, "n_words", &n_words) || n_words == 0u) {
        (void)snprintf(msg, sizeof(msg), "%s: n_words missing or zero", name);
        TEST_FAIL_MESSAGE(msg);
    }
    if (json_u32(&doc, "sample_rate_hz", &rate)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(g_chain.sample_rate_hz, rate, "capture sample rate is not this system's");
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
        free(words);
        (void)snprintf(msg, sizeof(msg), "%s: %u words declared, %u bytes read", pcm_name, (unsigned)n_words, (unsigned)got);
        TEST_FAIL_MESSAGE(msg);
    }

    truing_audio_source_if_t src;
    truing_audio_buffer_ctx_t sctx;
    truing_audio_buffer_init(&src, &sctx, words, n_words, NULL);
    truing_acoustic_if_t a;
    truing_acoustic_real_ctx_t ctx;
    const size_t bytes = truing_acoustic_real_scratch_bytes(&g_chain);
    void *scratch = malloc(bytes);
    TEST_ASSERT_NOT_NULL(scratch);
    const char *detail = NULL;
    TEST_ASSERT_TRUE_MESSAGE(truing_acoustic_real_init(&a, &ctx, g_clock, &g_chain, &g_excitation, &g_profile, &src, NULL, scratch, bytes, &detail),
                             detail);
    truing_tension_estimate_t e;
    truing_acoustic_real_analyze_words(&a, words, n_words, 1u, &e);
    const truing_acoustic_real_diag_t *d = &ctx.diag;

    char origin[32] = { 0 };
    (void)json_str(&doc, "expect_origin", origin, sizeof(origin));
    printf("  %-28s %6u words  onset %6u  window %6u+%-6u  f1 %8.3f Hz  snr %6.2f dB  %s/%s  %.1f N  [%s]\n",
           name, (unsigned)n_words, (unsigned)d->onset_sample, (unsigned)d->window.start_sample,
           (unsigned)d->window.n_samples, (double)d->f1_hz, (double)d->snr_db,
           truing_status_str(e.meta.status), truing_reason_str(e.meta.reason_code), (double)e.tension_n,
           origin[0] != '\0' ? origin : "no expectations");

    write_observed(dir, name, d, &e);

    /* ---- regression, where the bundle carries one ---- */
    unsigned n_checked = 0u;
    char s[48] = { 0 };
    uint32_t u = 0u;
    float f = 0.0f;
    if (json_str(&doc, "expect_status", s, sizeof(s))) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(s, truing_status_str(e.meta.status), name);
        n_checked++;
    }
    if (json_str(&doc, "expect_reason", s, sizeof(s))) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(s, truing_reason_str(e.meta.reason_code), name);
        n_checked++;
    }
    if (json_str(&doc, "expect_window_truncated_by", s, sizeof(s))) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(s, truing_window_truncation_str(d->window.truncated_by), name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_onsets", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->onsets, name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_onset_sample", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->onset_sample, name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_window_start_sample", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->window.start_sample, name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_window_n_samples", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->window.n_samples, name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_n_fft", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->n_fft, name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_n_strong_peaks", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->n_strong_peaks, name);
        n_checked++;
    }
    if (json_u32(&doc, "expect_n_peaks_in_band", &u)) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(u, d->n_peaks_in_band, name);
        n_checked++;
    }
    if (json_f32(&doc, "expect_f1_hz", &f)) {
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(tol_for(f), f, d->f1_hz, name);
        n_checked++;
    }
    if (json_f32(&doc, "expect_snr_db", &f)) {
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(tol_for(f), f, d->snr_db, name);
        n_checked++;
    }
    if (json_f32(&doc, "expect_tension_n", &f)) {
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(tol_for(f), f, e.tension_n, name);
        n_checked++;
    }
    *checked += n_checked;

    free(scratch);
    free(words);
    return n_checked > 0u;
}

static void test_every_recorded_capture_replays(void)
{
    if (!find_captures_dir(g_dir, sizeof(g_dir))) {
        TEST_FAIL_MESSAGE("no capture index found - expected test/fixtures/acoustic/captures/index.txt "
                          "(set TRUING_CAPTURES_DIR to replay a bundle elsewhere)");
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

    uint8_t digest[TRUING_SHA256_DIGEST_BYTES];
    char here[2u * TRUING_SHA256_DIGEST_BYTES + 1u];
    truing_chain_profile_digest(&g_chain, digest);
    hex_of(digest, sizeof(digest), here);
    printf("\nchain_digest of this build: %s\n", here);
    printf("replaying %u capture bundle(s) from %s\n", n, g_dir);
    if (n == 0u) {
        printf("  (index is empty - nothing to replay; capture one with tools/capture_fetch.py)\n");
        return;
    }
    unsigned checked = 0u, with_expectations = 0u;
    for (unsigned i = 0u; i < n; ++i) {
        if (replay_one(g_dir, names[i], &checked)) {
            with_expectations++;
        }
    }
    printf("  %u/%u bundle(s) carried expectations; %u assertions checked\n", with_expectations, n, checked);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_recorded_capture_replays);
    return UNITY_END();
}
