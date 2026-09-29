/* The bring-up liveness rule (truing_hal/audio_liveness.h): a microphone is alive when its samples
 * scatter, not when they are merely nonzero. The lines that fooled the old "nonzero" test are the point
 * of these tests: a floating data line stuck at -1, with mains-pickup glitches on it. */
#include <stdlib.h>
#include <unity.h>

#include "truing_hal/audio_liveness.h"

void setUp(void) {}
void tearDown(void) {}

#define N 9600u

static int32_t word(int32_t s24) { return (int32_t)((uint32_t)s24 << 8); }

static void test_a_scattering_signal_is_alive_and_not_stuck(void)
{
    static int32_t w[N];
    srand(7);
    for (uint32_t i = 0; i < N; ++i) w[i] = word((rand() % 1601) - 800);   /* a few hundred LSB of self-noise */
    truing_audio_liveness_t lv;
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_FALSE(lv.stuck);
    TEST_ASSERT_EQUAL_UINT32(0u, lv.mode_count);
    TEST_ASSERT_EQUAL_UINT32(0u, lv.rails);
}

static void test_a_line_stuck_at_minus_one_is_dead_although_every_word_is_nonzero(void)
{
    static int32_t w[N];
    for (uint32_t i = 0; i < N; ++i) w[i] = word(-1);   /* nonzero, and the old check passed it */
    truing_audio_liveness_t lv;
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_TRUE(lv.stuck);
    TEST_ASSERT_EQUAL_INT32(-1, lv.mode);
    TEST_ASSERT_EQUAL_UINT32(N, lv.mode_count);
}

static void test_a_floating_line_with_full_scale_glitches_is_still_dead(void)
{
    static int32_t w[N];
    for (uint32_t i = 0; i < N; ++i) w[i] = word(-1);
    for (uint32_t i = 0; i < N; i += 7) w[i] = word((i & 1u) ? 8388607 : -8388608);   /* pickup, clipping to the rails */
    truing_audio_liveness_t lv;
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_TRUE(lv.stuck);
    TEST_ASSERT_EQUAL_INT32(-1, lv.mode);
    TEST_ASSERT_TRUE(lv.rails > 1000u);   /* the glitches are counted, not mistaken for signal */
}

static void test_a_line_stuck_low_is_dead_and_names_zero(void)
{
    static int32_t w[N];
    for (uint32_t i = 0; i < N; ++i) w[i] = 0;
    truing_audio_liveness_t lv;
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_TRUE(lv.stuck);
    TEST_ASSERT_EQUAL_INT32(0, lv.mode);
}

static void test_a_live_mic_that_clips_is_not_stuck_but_reports_rails(void)
{
    static int32_t w[N];
    srand(3);
    for (uint32_t i = 0; i < N; ++i) w[i] = word((rand() % 4001) - 2000);
    for (uint32_t i = 0; i < 40u; ++i) w[i * 200u] = word(8388607);
    truing_audio_liveness_t lv;
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_FALSE(lv.stuck);
    TEST_ASSERT_EQUAL_UINT32(40u, lv.rails);
}

static void test_the_stuck_threshold_is_half_of_the_samples(void)
{
    static int32_t w[N];
    srand(11);
    for (uint32_t i = 0; i < N; ++i) w[i] = word(1000 + (int32_t)i);   /* all distinct */
    for (uint32_t i = 0; i < N * 6u / 10u; ++i) w[i] = word(5);         /* 60% one value */
    truing_audio_liveness_t lv;
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_TRUE(lv.stuck);
    TEST_ASSERT_EQUAL_INT32(5, lv.mode);
    for (uint32_t i = 0; i < N; ++i) w[i] = word(1000 + (int32_t)i);
    for (uint32_t i = 0; i < N * 4u / 10u; ++i) w[i] = word(5);         /* 40% one value: still a signal */
    truing_audio_liveness(w, N, &lv);
    TEST_ASSERT_FALSE(lv.stuck);
}

static void test_an_empty_or_missing_capture_is_not_alive(void)
{
    truing_audio_liveness_t lv;
    truing_audio_liveness(NULL, 0u, &lv);
    TEST_ASSERT_TRUE(lv.stuck);
    int32_t one = word(3);
    truing_audio_liveness(&one, 0u, &lv);
    TEST_ASSERT_TRUE(lv.stuck);
    truing_audio_liveness(&one, 1u, NULL);   /* no crash */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_scattering_signal_is_alive_and_not_stuck);
    RUN_TEST(test_a_line_stuck_at_minus_one_is_dead_although_every_word_is_nonzero);
    RUN_TEST(test_a_floating_line_with_full_scale_glitches_is_still_dead);
    RUN_TEST(test_a_line_stuck_low_is_dead_and_names_zero);
    RUN_TEST(test_a_live_mic_that_clips_is_not_stuck_but_reports_rails);
    RUN_TEST(test_the_stuck_threshold_is_half_of_the_samples);
    RUN_TEST(test_an_empty_or_missing_capture_is_not_alive);
    return UNITY_END();
}
