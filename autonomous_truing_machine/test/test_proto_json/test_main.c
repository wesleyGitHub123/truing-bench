/* Bounded JSON writer and reader (SPEC §12 wire protocol primitives). */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "truing_proto/json.h"

static char g_buf[512];

void setUp(void)
{
    memset(g_buf, 0, sizeof(g_buf));
}
void tearDown(void) {}

static const char *write_ok(truing_json_writer_t *w)
{
    size_t len = 0u;
    TEST_ASSERT_TRUE(truing_json_finish(w, &len));
    TEST_ASSERT_EQUAL_size_t(strlen(g_buf), len);
    return g_buf;
}

static void test_writer_nests_and_commas(void)
{
    truing_json_writer_t w;
    truing_json_init(&w, g_buf, sizeof(g_buf));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "a", "x");
    truing_json_u32(&w, "b", 7u);
    truing_json_obj_open(&w, "c");
    truing_json_bool(&w, "d", true);
    truing_json_obj_close(&w);
    truing_json_arr_open(&w, "e");
    truing_json_i32(&w, NULL, -3);
    truing_json_null(&w, NULL);
    truing_json_arr_close(&w);
    truing_json_obj_close(&w);
    TEST_ASSERT_EQUAL_STRING("{\"a\":\"x\",\"b\":7,\"c\":{\"d\":true},\"e\":[-3,null]}", write_ok(&w));
}

static void test_writer_escapes_and_hex(void)
{
    truing_json_writer_t w;
    truing_json_init(&w, g_buf, sizeof(g_buf));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "s", "q\"b\\s\nt\tc\x01");
    const uint8_t bytes[3] = { 0x00u, 0xABu, 0xFFu };
    truing_json_hex(&w, "h", bytes, sizeof(bytes));
    truing_json_obj_close(&w);
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"q\\\"b\\\\s\\nt\\tc\\u0001\",\"h\":\"00abff\"}", write_ok(&w));
}

static void test_writer_floats_and_non_finite(void)
{
    truing_json_writer_t w;
    truing_json_init(&w, g_buf, sizeof(g_buf));
    truing_json_obj_open(&w, NULL);
    truing_json_f32(&w, "a", 1.25f, 3u);
    truing_json_f32(&w, "b", -0.5f, 1u);
    truing_json_f32(&w, "c", 42.0f, 0u);
    /* JSON has no NaN or Infinity; both become null, which is what the value means. */
    truing_json_f32(&w, "nan", NAN, 3u);
    truing_json_f32(&w, "inf", INFINITY, 3u);
    truing_json_obj_close(&w);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1.250,\"b\":-0.5,\"c\":42,\"nan\":null,\"inf\":null}", write_ok(&w));
}

static void test_writer_overflow_is_latched_not_truncated(void)
{
    char small[16];
    truing_json_writer_t w;
    truing_json_init(&w, small, sizeof(small));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "key", "a value far too long for this buffer");
    truing_json_obj_close(&w);
    size_t len = 0u;
    /* A truncated frame is never handed back as if it were valid. */
    TEST_ASSERT_FALSE(truing_json_finish(&w, &len));
    TEST_ASSERT_EQUAL_size_t(0u, len);
    TEST_ASSERT_EQUAL_STRING("", small);
}

static void test_writer_rejects_unclosed_container(void)
{
    truing_json_writer_t w;
    truing_json_init(&w, g_buf, sizeof(g_buf));
    truing_json_obj_open(&w, NULL);
    truing_json_obj_open(&w, "inner");
    truing_json_obj_close(&w);
    size_t len = 0u;
    TEST_ASSERT_FALSE(truing_json_finish(&w, &len));   /* outer object still open */
}

static void test_writer_depth_cap(void)
{
    truing_json_writer_t w;
    truing_json_init(&w, g_buf, sizeof(g_buf));
    for (unsigned i = 0u; i <= TRUING_JSON_MAX_DEPTH; ++i) {
        truing_json_obj_open(&w, i == 0u ? NULL : "n");
    }
    size_t len = 0u;
    TEST_ASSERT_FALSE(truing_json_finish(&w, &len));
}

static bool doc_ok(const char *s)
{
    truing_json_doc_t d;
    return truing_json_doc_init(&d, s, strlen(s));
}

static void test_reader_accepts_valid_documents(void)
{
    TEST_ASSERT_TRUE(doc_ok("{}"));
    TEST_ASSERT_TRUE(doc_ok("  { \"a\" : 1 }  "));
    TEST_ASSERT_TRUE(doc_ok("{\"a\":[1,2,{\"b\":null}],\"c\":true}"));
    TEST_ASSERT_TRUE(doc_ok("{\"a\":-1.5e-3}"));
    TEST_ASSERT_TRUE(doc_ok("{\"a\":\"\\u00e9\\t\\\\\"}"));
}

static void test_reader_rejects_malformed_documents(void)
{
    TEST_ASSERT_FALSE(doc_ok(""));
    TEST_ASSERT_FALSE(doc_ok("[]"));                  /* root must be an object */
    TEST_ASSERT_FALSE(doc_ok("{"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":1"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":1}}"));          /* trailing garbage */
    TEST_ASSERT_FALSE(doc_ok("{\"a\":1,}"));
    TEST_ASSERT_FALSE(doc_ok("{a:1}"));               /* unquoted key */
    TEST_ASSERT_FALSE(doc_ok("{'a':1}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":01}"));          /* leading zero */
    TEST_ASSERT_FALSE(doc_ok("{\"a\":.5}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":1.}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":+1}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":1e}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":NaN}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":\"unterminated}"));
    TEST_ASSERT_FALSE(doc_ok("{\"a\":\"raw\nnewline\"}"));   /* control char in a string */
    TEST_ASSERT_FALSE(doc_ok("{\"a\":\"\\q\"}"));            /* invalid escape */
    TEST_ASSERT_FALSE(doc_ok("{\"a\":\"\\u12\"}"));          /* short \u */
    TEST_ASSERT_FALSE(doc_ok("{\"a\":tru}"));
}

static void test_reader_depth_cap_rejects_deep_nesting(void)
{
    /* Bounded recursion is what keeps a hostile frame off the task stack. */
    char deep[64];
    size_t n = 0u;
    deep[n++] = '{';
    deep[n++] = '"';
    deep[n++] = 'a';
    deep[n++] = '"';
    deep[n++] = ':';
    for (unsigned i = 0u; i < TRUING_JSON_MAX_DEPTH + 2u; ++i) {
        deep[n++] = '[';
    }
    for (unsigned i = 0u; i < TRUING_JSON_MAX_DEPTH + 2u; ++i) {
        deep[n++] = ']';
    }
    deep[n++] = '}';
    truing_json_doc_t d;
    TEST_ASSERT_FALSE(truing_json_doc_init(&d, deep, n));
}

static void test_reader_gets_members(void)
{
    const char *src = "{\"s\":\"hi\",\"n\":-2.5,\"b\":false,\"z\":null,\"o\":{\"k\":1},\"a\":[1]}";
    truing_json_doc_t d;
    TEST_ASSERT_TRUE(truing_json_doc_init(&d, src, strlen(src)));

    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&d, "s", &v));
    TEST_ASSERT_TRUE(truing_json_str_equals(&v, "hi"));
    TEST_ASSERT_FALSE(truing_json_str_equals(&v, "hix"));
    TEST_ASSERT_FALSE(truing_json_str_equals(&v, "h"));

    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NUMBER, truing_json_get(&d, "n", &v));
    TEST_ASSERT_FLOAT_WITHIN(1e-9f, -2.5f, (float)v.number);
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_BOOL, truing_json_get(&d, "b", &v));
    TEST_ASSERT_FALSE(v.boolean);
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NULL, truing_json_get(&d, "z", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_OBJECT, truing_json_get(&d, "o", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_ARRAY, truing_json_get(&d, "a", &v));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_ABSENT, truing_json_get(&d, "missing", &v));
}

static void test_reader_decodes_escapes(void)
{
    const char *src = "{\"k\\u0065y\":\"a\\\"b\\\\c\\td\\u0041\"}";
    truing_json_doc_t d;
    TEST_ASSERT_TRUE(truing_json_doc_init(&d, src, strlen(src)));
    truing_json_value_t v;
    /* The key itself is escaped; lookup decodes it rather than comparing raw bytes. */
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&d, "key", &v));
    char out[32];
    TEST_ASSERT_TRUE(truing_json_copy_str(&v, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\tdA", out);

    char tiny[4];
    TEST_ASSERT_FALSE(truing_json_copy_str(&v, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

static void test_reader_first_duplicate_key_wins(void)
{
    /* A later duplicate must not be able to override a value already established. */
    const char *src = "{\"cmd\":\"ABORT\",\"cmd\":\"START_TRUING\"}";
    truing_json_doc_t d;
    TEST_ASSERT_TRUE(truing_json_doc_init(&d, src, strlen(src)));
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&d, "cmd", &v));
    TEST_ASSERT_TRUE(truing_json_str_equals(&v, "ABORT"));
}

static void test_writer_output_reparses(void)
{
    truing_json_writer_t w;
    truing_json_init(&w, g_buf, sizeof(g_buf));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "text", "quote\" backslash\\ tab\t");
    truing_json_f32(&w, "v", 3.14159f, 4u);
    truing_json_arr_open(&w, "list");
    truing_json_u32(&w, NULL, 1u);
    truing_json_u32(&w, NULL, 2u);
    truing_json_arr_close(&w);
    truing_json_obj_close(&w);
    size_t len = 0u;
    TEST_ASSERT_TRUE(truing_json_finish(&w, &len));

    truing_json_doc_t d;
    TEST_ASSERT_TRUE(truing_json_doc_init(&d, g_buf, len));
    truing_json_value_t v;
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_STRING, truing_json_get(&d, "text", &v));
    TEST_ASSERT_TRUE(truing_json_str_equals(&v, "quote\" backslash\\ tab\t"));
    TEST_ASSERT_EQUAL_INT(TRUING_JSON_NUMBER, truing_json_get(&d, "v", &v));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.14159f, (float)v.number);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_writer_nests_and_commas);
    RUN_TEST(test_writer_escapes_and_hex);
    RUN_TEST(test_writer_floats_and_non_finite);
    RUN_TEST(test_writer_overflow_is_latched_not_truncated);
    RUN_TEST(test_writer_rejects_unclosed_container);
    RUN_TEST(test_writer_depth_cap);
    RUN_TEST(test_reader_accepts_valid_documents);
    RUN_TEST(test_reader_rejects_malformed_documents);
    RUN_TEST(test_reader_depth_cap_rejects_deep_nesting);
    RUN_TEST(test_reader_gets_members);
    RUN_TEST(test_reader_decodes_escapes);
    RUN_TEST(test_reader_first_duplicate_key_wins);
    RUN_TEST(test_writer_output_reparses);
    return UNITY_END();
}
