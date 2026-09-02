/* SHA-256 and the influence-artifact loader: the three SPEC 8.7 checks, R3 limits and expansion. */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "truing/artifact.h"
#include "truing/sha256.h"
#include "truing_fixtures/fixtures.h"

static truing_artifact_t g_art;          /* ~50 KB: static */
static uint8_t g_buf[40000];
static truing_wheel_class_config_t g_wheel;
static truing_solver_config_t g_solver;

void setUp(void)
{
    truing_fixture_wheel_class_sym32(&g_wheel);
    truing_fixture_solver_config(&g_solver, 32u);
    TEST_ASSERT_TRUE(fixture_sym32_artifact_blob_len <= sizeof(g_buf));
    memcpy(g_buf, fixture_sym32_artifact_blob, fixture_sym32_artifact_blob_len);
}
void tearDown(void) {}

static void hex(const uint8_t *d, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2u * i] = digits[d[i] >> 4];
        out[2u * i + 1u] = digits[d[i] & 0x0Fu];
    }
    out[2u * n] = '\0';
}

static void test_sha256_known_vectors(void)
{
    uint8_t d[32];
    char h[65];
    truing_sha256((const uint8_t *)"", 0u, d);
    hex(d, 32u, h);
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", h);
    truing_sha256((const uint8_t *)"abc", 3u, d);
    hex(d, 32u, h);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    truing_sha256((const uint8_t *)two, strlen(two), d);
    hex(d, 32u, h);
    TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
    /* streaming in odd chunks equals one-shot */
    truing_sha256_t ctx;
    truing_sha256_init(&ctx);
    truing_sha256_update(&ctx, (const uint8_t *)two, 7u);
    truing_sha256_update(&ctx, (const uint8_t *)two + 7, strlen(two) - 7u);
    uint8_t d2[32];
    truing_sha256_final(&ctx, d2);
    TEST_ASSERT_EQUAL_MEMORY(d, d2, 32u);
    /* a million 'a' (FIPS 180-4 test) */
    static uint8_t a1000[1000];
    memset(a1000, 'a', sizeof(a1000));
    truing_sha256_init(&ctx);
    for (int i = 0; i < 1000; ++i) truing_sha256_update(&ctx, a1000, sizeof(a1000));
    truing_sha256_final(&ctx, d);
    hex(d, 32u, h);
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", h);
}

static void test_fingerprint_hex_round_trip(void)
{
    truing_fingerprint_t fp;
    TEST_ASSERT_TRUE(truing_fingerprint_from_hex(fixture_sym32_artifact_fingerprint_hex, &fp));
    TEST_ASSERT_TRUE(fp.set);
    char back[65];
    truing_fingerprint_hex(&fp, back);
    TEST_ASSERT_EQUAL_STRING(fixture_sym32_artifact_fingerprint_hex, back);
    TEST_ASSERT_FALSE(truing_fingerprint_from_hex("zz", &fp));
    TEST_ASSERT_FALSE(fp.set);
}

static void test_golden_artifact_loads_and_expands(void)
{
    const char *detail = NULL;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_OK, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &g_solver, &g_art, &detail));
    TEST_ASSERT_TRUE(g_art.loaded);
    TEST_ASSERT_EQUAL_UINT32(1u, g_art.artifact_id);
    TEST_ASSERT_EQUAL_UINT8(32u, g_art.n_spokes);
    TEST_ASSERT_EQUAL_UINT8(32u, g_art.n_rim_angles);
    TEST_ASSERT_EQUAL_UINT8(g_solver.N_lat, g_art.N_lat);
    TEST_ASSERT_EQUAL_UINT8(g_solver.N_rad, g_art.N_rad);
    char h[65];
    hex(g_art.content_hash, 32u, h);
    TEST_ASSERT_EQUAL_STRING(fixture_sym32_artifact_content_hash_hex, h);
    truing_fingerprint_hex(&g_art.generating_fingerprint, h);
    TEST_ASSERT_EQUAL_STRING(fixture_sym32_artifact_fingerprint_hex, h);
    /* Phase 1b finding: the symmetric fixture's common mode is NOT identified (condition (i) fails
     * on the uniform radial contraction), so the artifact must say so and carry T_target anyway. */
    TEST_ASSERT_FALSE(g_art.n_mt_identified);
    TEST_ASSERT_TRUE(g_art.T_target_present);
    TEST_ASSERT_FALSE(g_art.asymmetric);
    TEST_ASSERT_EQUAL_UINT8(0u, g_art.displacement_null_dim);
    TEST_ASSERT_TRUE(isfinite(g_art.cond_i_residual) && g_art.cond_i_residual > g_art.cond_i_tolerance);
    const truing_artifact_layout_t *F = truing_artifact_layout(&g_art, TRUING_LAYOUT_FULL);
    const truing_artifact_layout_t *T = truing_artifact_layout(&g_art, TRUING_LAYOUT_TENSION_ABSENT);
    TEST_ASSERT_NOT_NULL(F);
    TEST_ASSERT_NOT_NULL(T);
    TEST_ASSERT_EQUAL_UINT16(96u, F->n_rows);
    TEST_ASSERT_EQUAL_UINT16(64u, T->n_rows);
    TEST_ASSERT_EQUAL_UINT8(32u, F->effective_rank);
    TEST_ASSERT_EQUAL_UINT8(32u, T->effective_rank);
    TEST_ASSERT_TRUE(F->effective_condition_number > 1.0f && F->effective_condition_number <= g_solver.max_condition_number);
    TEST_ASSERT_TRUE(T->effective_condition_number > 1.0f && T->effective_condition_number <= g_solver.max_condition_number);
    /* Expansion sanity: lateral influence is largest near the adjusted spoke and has both signs
     * (SPEC 6.4: tightening a Side-B spoke pulls the rim toward Side B; alternate sides alternate sign). */
    float max_abs = 0.0f;
    uint8_t max_k = 0u;
    for (uint8_t k = 0; k < 32u; ++k) {
        if (fabsf(g_art.phi_u[k][0]) > max_abs) {
            max_abs = fabsf(g_art.phi_u[k][0]);
            max_k = k;
        }
    }
    TEST_ASSERT_TRUE(max_k == 0u || max_k == 31u || max_k == 1u);
    TEST_ASSERT_TRUE(g_art.phi_u[0][0] > 0.0f);        /* spoke 0 is Side B: positive lateral (toward B) */
    TEST_ASSERT_TRUE(g_art.phi_u[1][1] < 0.0f);        /* spoke 1 is Side A */
    TEST_ASSERT_TRUE(g_art.phi_v[0][0] < 0.0f);        /* tightening pulls the rim inward; system radial is + outward (SPEC 6.4) */
}

static void test_integrity_check_rejects_a_flipped_bit(void)
{
    g_buf[TRUING_ARTIFACT_HEADER_BYTES + 1000u] ^= 0x01u;
    const char *detail = NULL;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_INTEGRITY, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &g_solver, &g_art, &detail));
    TEST_ASSERT_FALSE(g_art.loaded);
    TEST_ASSERT_EQUAL_STRING("content_hash", detail);
}

static void test_compatibility_check_uses_the_configured_expectation(void)
{
    g_wheel.expected_influence_fingerprint.bytes[5] ^= 0xFFu;
    const char *detail = NULL;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_INCOMPATIBLE, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &g_solver, &g_art, &detail));
    TEST_ASSERT_FALSE(g_art.loaded);
    g_wheel.expected_influence_fingerprint.set = false;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_INCOMPATIBLE, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &g_solver, &g_art, &detail));
}

static void test_shape_check_never_reshapes(void)
{
    const char *detail = NULL;
    truing_solver_config_t s = g_solver;
    s.N_lat = (uint8_t)(g_solver.N_lat + 1u);
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_SHAPE, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &s, &g_art, &detail));
    truing_wheel_class_config_t w = g_wheel;
    w.n_spokes = 36u;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_SHAPE, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &w, &g_solver, &g_art, &detail));
    s = g_solver;
    s.trust_radial = 0.25f;                       /* the pseudoinverses were computed for 0.5 */
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_SHAPE, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &s, &g_art, &detail));
    w = g_wheel;
    w.c_side_a_n_per_rev = 500.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_SHAPE, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &w, &g_solver, &g_art, &detail));
    /* truncated buffers */
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_TRUNCATED, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len - 1u, &g_wheel, &g_solver, &g_art, &detail));
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_TRUNCATED, truing_artifact_load(g_buf, 10u, &g_wheel, &g_solver, &g_art, &detail));
    g_buf[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_MAGIC, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &g_solver, &g_art, &detail));
}

static void test_r3_conditioning_limit_is_enforced_at_load(void)
{
    const char *detail = NULL;
    truing_solver_config_t s = g_solver;
    s.max_condition_number = 1.5f;
    TEST_ASSERT_EQUAL_INT(TRUING_ART_ERR_CONDITIONING, truing_artifact_load(g_buf, fixture_sym32_artifact_blob_len, &g_wheel, &s, &g_art, &detail));
    TEST_ASSERT_FALSE(g_art.loaded);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha256_known_vectors);
    RUN_TEST(test_fingerprint_hex_round_trip);
    RUN_TEST(test_golden_artifact_loads_and_expands);
    RUN_TEST(test_integrity_check_rejects_a_flipped_bit);
    RUN_TEST(test_compatibility_check_uses_the_configured_expectation);
    RUN_TEST(test_shape_check_never_reshapes);
    RUN_TEST(test_r3_conditioning_limit_is_enforced_at_load);
    return UNITY_END();
}
