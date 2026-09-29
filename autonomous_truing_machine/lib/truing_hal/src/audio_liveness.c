#include "truing_hal/audio_liveness.h"

#include <stddef.h>

#define FULL_SCALE 8388608   /* 2^23: the 24-bit sample range */

void truing_audio_liveness(const int32_t *words, uint32_t n, truing_audio_liveness_t *out)
{
    if (out == NULL) {
        return;
    }
    out->n = n;
    out->mode = 0;
    out->mode_count = 0u;
    out->stuck = true;
    out->rails = 0u;
    if (words == NULL || n == 0u) {
        return;
    }
    /* Boyer-Moore majority vote: if any value fills more than half the samples it is the candidate,
     * found in one pass with no histogram. The count that follows confirms it. */
    int32_t candidate = 0;
    uint32_t votes = 0u;
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t s24 = words[i] >> 8;
        if (s24 >= FULL_SCALE - 1 || s24 <= -FULL_SCALE) {
            out->rails++;
        }
        if (votes == 0u) {
            candidate = s24;
            votes = 1u;
        } else if (s24 == candidate) {
            votes++;
        } else {
            votes--;
        }
    }
    uint32_t count = 0u;
    for (uint32_t i = 0; i < n; ++i) {
        if ((words[i] >> 8) == candidate) {
            count++;
        }
    }
    out->stuck = (uint64_t)count * 2u >= (uint64_t)n;
    if (out->stuck) {
        out->mode = candidate;
        out->mode_count = count;
    }
}
