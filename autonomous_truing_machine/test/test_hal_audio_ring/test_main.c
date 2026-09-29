/* The front end's ring and input split (truing_hal/audio_ring.h): the exact code the I2S drain
 * task runs, on the host. One input must behave as the ring always has -- the stream as delivered
 * -- and two interleaved inputs must land sample-aligned in their own rings, across wrap-around. */
#include <string.h>
#include <unity.h>

#include "truing_hal/audio_ring.h"

void setUp(void) {}
void tearDown(void) {}

static void test_one_input_ring_is_the_stream_as_delivered_across_wraparound(void)
{
    int32_t mem[8];
    truing_audio_ring_t r = { .ring = { mem, NULL }, .cap = 8u, .n_inputs = 1u, .head = 0u, .filled = 0u };
    int32_t in[11];
    for (int i = 0; i < 11; ++i) in[i] = 100 + i;
    TEST_ASSERT_EQUAL_UINT32(6u, truing_audio_ring_append(&r, in, 6u));
    TEST_ASSERT_EQUAL_UINT32(5u, truing_audio_ring_append(&r, in + 6, 5u));   /* wraps */
    TEST_ASSERT_EQUAL_UINT32(8u, r.filled);
    TEST_ASSERT_EQUAL_UINT32(3u, r.head);
    int32_t tail[5];
    truing_audio_ring_tail(&r, 0u, r.head, 5u, tail);
    for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_INT32(106 + i, tail[i]);
}

static void test_two_inputs_split_into_aligned_rings(void)
{
    int32_t left[6], right[6];
    truing_audio_ring_t r = { .ring = { left, right }, .cap = 6u, .n_inputs = 2u, .head = 0u, .filled = 0u };
    /* frames: (L=1000+f, R=-(1000+f)), 9 frames, so the ring wraps */
    int32_t in[18];
    for (int f = 0; f < 9; ++f) {
        in[2 * f] = 1000 + f;
        in[2 * f + 1] = -(1000 + f);
    }
    TEST_ASSERT_EQUAL_UINT32(4u, truing_audio_ring_append(&r, in, 8u));
    TEST_ASSERT_EQUAL_UINT32(5u, truing_audio_ring_append(&r, in + 8, 10u));
    TEST_ASSERT_EQUAL_UINT32(6u, r.filled);
    int32_t tl[4], tr[4];
    truing_audio_ring_tail(&r, 0u, r.head, 4u, tl);
    truing_audio_ring_tail(&r, 1u, r.head, 4u, tr);
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT32(1005 + i, tl[i]);
        TEST_ASSERT_EQUAL_INT32(-(1005 + i), tr[i]);   /* same instant, other microphone */
    }
}

static void test_a_trailing_partial_frame_is_not_appended(void)
{
    int32_t left[4], right[4];
    truing_audio_ring_t r = { .ring = { left, right }, .cap = 4u, .n_inputs = 2u, .head = 0u, .filled = 0u };
    const int32_t in[5] = { 1, -1, 2, -2, 3 };
    TEST_ASSERT_EQUAL_UINT32(2u, truing_audio_ring_append(&r, in, 5u));
    TEST_ASSERT_EQUAL_UINT32(2u, r.head);
}

static void test_frames_extract_picks_one_input_and_copies_mono_unchanged(void)
{
    const int32_t frames[6] = { 10, 20, 11, 21, 12, 22 };
    int32_t out[3];
    truing_audio_frames_extract(frames, 3u, 2u, 1u, out);
    TEST_ASSERT_EQUAL_INT32(20, out[0]);
    TEST_ASSERT_EQUAL_INT32(21, out[1]);
    TEST_ASSERT_EQUAL_INT32(22, out[2]);
    truing_audio_frames_extract(frames, 3u, 2u, 0u, out);
    TEST_ASSERT_EQUAL_INT32(10, out[0]);
    TEST_ASSERT_EQUAL_INT32(12, out[2]);
    int32_t mono[6];
    truing_audio_frames_extract(frames, 6u, 1u, 0u, mono);
    TEST_ASSERT_EQUAL_MEMORY(frames, mono, sizeof(frames));
}

static void test_read_since_counts_a_gap_instead_of_splicing_it(void)
{
    int32_t left[8], right[8];
    truing_audio_ring_t r = { .ring = { left, right }, .cap = 8u, .n_inputs = 2u, .head = 0u, .filled = 0u };
    int32_t in[20];
    for (int f = 0; f < 10; ++f) {
        in[2 * f] = 500 + f;
        in[2 * f + 1] = -(500 + f);
    }
    uint32_t pos = r.head, lost = 0u;
    int32_t out[8];
    TEST_ASSERT_EQUAL_UINT32(0u, truing_audio_ring_read_since(&r, 1u, &pos, 8u, out, &lost));   /* nothing yet */
    truing_audio_ring_append(&r, in, 10u);   /* frames 0..4 */
    TEST_ASSERT_EQUAL_UINT32(3u, truing_audio_ring_read_since(&r, 1u, &pos, 3u, out, &lost));   /* max honoured */
    for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_INT32(-(500 + i), out[i]);
    truing_audio_ring_append(&r, in + 10, 10u);   /* frames 5..9, the head wraps */
    /* 7 frames outstanding is past the lap margin (cap 8 -> 6): the reader is moved up to 4 behind the
     * head and the 3 skipped frames (3, 4, 5) are reported */
    TEST_ASSERT_EQUAL_UINT32(4u, truing_audio_ring_read_since(&r, 1u, &pos, 4u, out, &lost));
    TEST_ASSERT_EQUAL_UINT32(3u, lost);
    TEST_ASSERT_EQUAL_INT32(-(500 + 6), out[0]);
    TEST_ASSERT_EQUAL_INT32(-(500 + 9), out[3]);
    TEST_ASSERT_EQUAL_UINT32(0u, truing_audio_ring_read_since(&r, 1u, &pos, 4u, out, &lost));   /* caught up */
    TEST_ASSERT_EQUAL_UINT32(3u, lost);
}

static void test_read_since_keeps_up_when_polled_often_and_lists_no_loss(void)
{
    int32_t mem[8];
    truing_audio_ring_t r = { .ring = { mem, NULL }, .cap = 8u, .n_inputs = 1u, .head = 0u, .filled = 0u };
    uint32_t pos = r.head, lost = 0u, next = 0u;
    int32_t out[8];
    for (int round = 0; round < 10; ++round) {
        int32_t in[3];
        for (int i = 0; i < 3; ++i) in[i] = (int32_t)(next + (uint32_t)i);
        truing_audio_ring_append(&r, in, 3u);
        const uint32_t n = truing_audio_ring_read_since(&r, 0u, &pos, 8u, out, &lost);
        TEST_ASSERT_EQUAL_UINT32(3u, n);
        for (uint32_t i = 0; i < n; ++i) TEST_ASSERT_EQUAL_INT32((int32_t)(next + i), out[i]);
        next += 3u;
    }
    TEST_ASSERT_EQUAL_UINT32(0u, lost);
}

static void test_read_since_refuses_bad_arguments(void)
{
    int32_t mem[4];
    truing_audio_ring_t r = { .ring = { mem, NULL }, .cap = 4u, .n_inputs = 1u, .head = 0u, .filled = 0u };
    uint32_t pos = 9u;   /* outside the ring */
    int32_t out[4];
    TEST_ASSERT_EQUAL_UINT32(0u, truing_audio_ring_read_since(&r, 0u, &pos, 4u, out, NULL));
    pos = 0u;
    TEST_ASSERT_EQUAL_UINT32(0u, truing_audio_ring_read_since(&r, 1u, &pos, 4u, out, NULL));   /* no such input */
    TEST_ASSERT_EQUAL_UINT32(0u, truing_audio_ring_read_since(NULL, 0u, &pos, 4u, out, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_read_since_counts_a_gap_instead_of_splicing_it);
    RUN_TEST(test_read_since_keeps_up_when_polled_often_and_lists_no_loss);
    RUN_TEST(test_read_since_refuses_bad_arguments);
    RUN_TEST(test_one_input_ring_is_the_stream_as_delivered_across_wraparound);
    RUN_TEST(test_two_inputs_split_into_aligned_rings);
    RUN_TEST(test_a_trailing_partial_frame_is_not_appended);
    RUN_TEST(test_frames_extract_picks_one_input_and_copies_mono_unchanged);
    return UNITY_END();
}
