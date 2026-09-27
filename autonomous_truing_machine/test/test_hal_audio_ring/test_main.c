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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_input_ring_is_the_stream_as_delivered_across_wraparound);
    RUN_TEST(test_two_inputs_split_into_aligned_rings);
    RUN_TEST(test_a_trailing_partial_frame_is_not_appended);
    RUN_TEST(test_frames_extract_picks_one_input_and_copies_mono_unchanged);
    return UNITY_END();
}
