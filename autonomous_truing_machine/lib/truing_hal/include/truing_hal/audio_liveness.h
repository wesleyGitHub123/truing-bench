/**
 * @file audio_liveness.h
 * Whether a microphone's capture is a live signal, as pure C so the host tests exercise the exact code
 * bring-up runs on target.
 *
 * "Nonzero" is not "alive". A data line nobody drives reads one constant -- 0 when it sits low, -1 when
 * it sits high -- and pickup on the floating wire adds isolated glitches that can even reach full scale.
 * A powered INMP441's self-noise is hundreds of LSB and never repeats one value that often, so the test
 * is whether the samples scatter: is one value more than half of them? This is the same rule as the
 * host workbench's mic_health() (tools/wb_analysis.py, STUCK_FRACTION 0.5).
 */
#ifndef TRUING_HAL_AUDIO_LIVENESS_H
#define TRUING_HAL_AUDIO_LIVENESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t n;            /* samples examined */
    int32_t  mode;         /* the value that fills at least half of them, when stuck (24-bit signed) */
    uint32_t mode_count;   /* how many samples hold it (0 when not stuck) */
    bool     stuck;        /* one value fills at least half of the samples: nothing is driving the line */
    uint32_t rails;        /* samples at full scale (the floating-line glitches, or a clipping mic) */
} truing_audio_liveness_t;

/* `words` are capture words (a 24-bit sample in the top bits of an int32). An empty capture is stuck
 * with mode 0 and no rails: there is no signal. O(n), no allocation. */
void truing_audio_liveness(const int32_t *words, uint32_t n, truing_audio_liveness_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_AUDIO_LIVENESS_H */
