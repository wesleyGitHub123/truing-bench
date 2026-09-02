/* Versioned, CRC-checked configuration serialization (SPEC §11.5, P7). */
#include <string.h>
#include <unity.h>

#include "truing/config_blob.h"
#include "truing/crc32.h"
#include "truing_fixtures/fixtures.h"

void setUp(void) {}
void tearDown(void) {}

static void test_crc32_known_vector(void)
{
    /* The standard CRC-32 check value. */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, truing_crc32((const uint8_t *)"123456789", 9u));
    uint32_t s = truing_crc32_update(0xFFFFFFFFu, (const uint8_t *)"1234", 4u);
    s = truing_crc32_update(s, (const uint8_t *)"56789", 5u);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, truing_crc32_final(s));
}

static void test_header_layout(void)
{
    truing_wheel_class_config_t w;
    uint8_t buf[TRUING_BLOB_MAX_BYTES];
    truing_fixture_wheel_class_sym32(&w);
    const size_t n = truing_blob_encode_wheel_class(&w, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > TRUING_BLOB_HEADER_BYTES);
    TEST_ASSERT_EQUAL_MEMORY("TRCF", buf, 4u);
    TEST_ASSERT_EQUAL_UINT8(TRUING_BLOB_SCHEMA_VERSION, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(TRUING_BLOB_KIND_WHEEL_CLASS, buf[6]);
    const uint16_t payload_len = (uint16_t)(buf[8] | (buf[9] << 8));
    TEST_ASSERT_EQUAL_UINT16(n - TRUING_BLOB_HEADER_BYTES, payload_len);
    TEST_ASSERT_EQUAL_UINT8(32u, buf[TRUING_BLOB_HEADER_BYTES]);   /* first payload byte: n_spokes */
    truing_blob_kind_t kind = TRUING_BLOB_KIND_UNSET;
    uint16_t plen = 0u;
    uint32_t crc = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_peek(buf, n, &kind, &plen, &crc));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_KIND_WHEEL_CLASS, kind);
    TEST_ASSERT_EQUAL_HEX32(truing_crc32(buf + TRUING_BLOB_HEADER_BYTES, plen), crc);
}

static void test_round_trip_all_kinds(void)
{
    uint8_t a[TRUING_BLOB_MAX_BYTES], b[TRUING_BLOB_MAX_BYTES];
    size_t na, nb;

    truing_wheel_class_config_t w, w2;
    truing_fixture_wheel_class_asym36(&w);
    na = truing_blob_encode_wheel_class(&w, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_wheel_class(a, na, &w2));
    nb = truing_blob_encode_wheel_class(&w2, b, sizeof(b));
    TEST_ASSERT_EQUAL_UINT32(na, nb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
    TEST_ASSERT_EQUAL_UINT8(36u, w2.n_spokes);
    TEST_ASSERT_TRUE(w2.asymmetric);
    TEST_ASSERT_EQUAL_INT(TRUING_SIDE_A_BASIS_ROTOR, w2.side_a_basis);
    TEST_ASSERT_TRUE(truing_fingerprint_equals(&w.expected_influence_fingerprint, &w2.expected_influence_fingerprint));
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_wheel_class_config_check(&w2, NULL));

    truing_solver_config_t s, s2;
    truing_fixture_solver_config(&s, 36u);
    na = truing_blob_encode_solver(&s, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_solver(a, na, &s2));
    nb = truing_blob_encode_solver(&s2, b, sizeof(b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
    TEST_ASSERT_EQUAL_UINT16(500u, s2.excitation_settle_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, 1.0e-5f, s2.trust_tension);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_solver_config_check(&s2, NULL));

    truing_chain_profile_t c, c2;
    truing_fixture_chain_profile_inmp441(&c);
    na = truing_blob_encode_chain_profile(&c, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_chain_profile(a, na, &c2));
    nb = truing_blob_encode_chain_profile(&c2, b, sizeof(b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
    TEST_ASSERT_EQUAL_UINT32(48000u, c2.sample_rate_hz);

    truing_tension_model_profile_t t, t2;
    truing_fixture_tension_model_profile_incomplete(&t);   /* carries NaN + UNESTABLISHED fields */
    na = truing_blob_encode_tension_model_profile(&t, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_tension_model_profile(a, na, &t2));
    nb = truing_blob_encode_tension_model_profile(&t2, b, sizeof(b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
    TEST_ASSERT_EQUAL_INT(TRUING_PROVENANCE_UNESTABLISHED, t2.L_eff_m.provenance);
    uint32_t missing = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_ERR_CALIBRATION_MISSING, truing_tension_model_profile_check(&t2, &missing, NULL));

    truing_machine_profile_t m, m2;
    truing_fixture_machine_profile(&m);
    na = truing_blob_encode_machine_profile(&m, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_machine_profile(a, na, &m2));
    nb = truing_blob_encode_machine_profile(&m2, b, sizeof(b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_ACOUSTIC, m2.reference_station);
    TEST_ASSERT_FALSE(m2.stations[TRUING_STATION_REFERENCE].present);
    TEST_ASSERT_EQUAL_INT(TRUING_CFG_OK, truing_machine_profile_check(&m2, NULL));
}

static void test_integrity_and_structure_failures(void)
{
    truing_solver_config_t s, out;
    uint8_t buf[TRUING_BLOB_MAX_BYTES];
    truing_fixture_solver_config(&s, 32u);
    const size_t n = truing_blob_encode_solver(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0u);

    /* Bit flip in the payload -> CRC mismatch, never a reinterpretation. */
    buf[TRUING_BLOB_HEADER_BYTES + 3u] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_CRC, truing_blob_decode_solver(buf, n, &out));
    buf[TRUING_BLOB_HEADER_BYTES + 3u] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_solver(buf, n, &out));

    /* Truncated / over-long. */
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_TRUNCATED, truing_blob_decode_solver(buf, n - 1u, &out));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_TRUNCATED, truing_blob_decode_solver(buf, 5u, &out));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_PAYLOAD_LENGTH, truing_blob_decode_solver(buf, n + 1u, &out));

    /* Wrong kind for the decoder. */
    truing_wheel_class_config_t w;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_KIND, truing_blob_decode_wheel_class(buf, n, &w));

    /* Bad magic / version / kind byte. */
    buf[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_MAGIC, truing_blob_decode_solver(buf, n, &out));
    buf[0] ^= 0xFFu;
    buf[4] = 9u;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_VERSION, truing_blob_decode_solver(buf, n, &out));
    buf[4] = TRUING_BLOB_SCHEMA_VERSION;
    buf[6] = 0u;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_KIND, truing_blob_decode_solver(buf, n, &out));
    buf[6] = TRUING_BLOB_KIND_SOLVER;
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_OK, truing_blob_decode_solver(buf, n, &out));

    /* A payload whose declared length matches the header but not the schema. */
    uint8_t shortbuf[TRUING_BLOB_MAX_BYTES];
    memcpy(shortbuf, buf, n);
    const uint16_t fake_len = (uint16_t)(n - TRUING_BLOB_HEADER_BYTES - 4u);
    shortbuf[8] = (uint8_t)(fake_len & 0xFFu);
    shortbuf[9] = (uint8_t)(fake_len >> 8);
    const uint32_t crc = truing_crc32(shortbuf + TRUING_BLOB_HEADER_BYTES, fake_len);
    shortbuf[10] = (uint8_t)(crc & 0xFFu);
    shortbuf[11] = (uint8_t)((crc >> 8) & 0xFFu);
    shortbuf[12] = (uint8_t)((crc >> 16) & 0xFFu);
    shortbuf[13] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_TRUNCATED, truing_blob_decode_solver(shortbuf, TRUING_BLOB_HEADER_BYTES + fake_len, &out));

    /* Encoder capacity. */
    TEST_ASSERT_EQUAL_UINT32(0u, truing_blob_encode_solver(&s, buf, 10u));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_blob_encode_solver(&s, buf, n - 1u));
    TEST_ASSERT_EQUAL_UINT32(n, truing_blob_encode_solver(&s, buf, n));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_blob_encode_solver(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(TRUING_BLOB_ERR_NULL, truing_blob_decode_solver(NULL, n, &out));
    TEST_ASSERT_EQUAL_STRING("ERR_CRC", truing_blob_result_str(TRUING_BLOB_ERR_CRC));
    TEST_ASSERT_EQUAL_STRING("MACHINE_PROFILE", truing_blob_kind_str(TRUING_BLOB_KIND_MACHINE_PROFILE));
}

static void test_every_blob_fits_the_capacity_bound(void)
{
    uint8_t buf[TRUING_BLOB_MAX_BYTES];
    truing_wheel_class_config_t w;
    truing_solver_config_t s;
    truing_chain_profile_t c;
    truing_tension_model_profile_t t;
    truing_machine_profile_t m;
    truing_fixture_wheel_class_sym32(&w);
    truing_fixture_solver_config(&s, 32u);
    truing_fixture_chain_profile_inmp441(&c);
    truing_fixture_tension_model_profile_complete(&t);
    truing_fixture_machine_profile(&m);
    TEST_ASSERT_TRUE(truing_blob_encode_wheel_class(&w, buf, sizeof(buf)) > 0u);
    TEST_ASSERT_TRUE(truing_blob_encode_solver(&s, buf, sizeof(buf)) > 0u);
    TEST_ASSERT_TRUE(truing_blob_encode_chain_profile(&c, buf, sizeof(buf)) > 0u);
    TEST_ASSERT_TRUE(truing_blob_encode_tension_model_profile(&t, buf, sizeof(buf)) > 0u);
    TEST_ASSERT_TRUE(truing_blob_encode_machine_profile(&m, buf, sizeof(buf)) > 0u);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_crc32_known_vector);
    RUN_TEST(test_header_layout);
    RUN_TEST(test_round_trip_all_kinds);
    RUN_TEST(test_integrity_and_structure_failures);
    RUN_TEST(test_every_blob_fits_the_capacity_bound);
    return UNITY_END();
}
