#include "truing_hal/audio_ring.h"

#include <string.h>

uint32_t truing_audio_ring_append(truing_audio_ring_t *r, const int32_t *words, uint32_t n_words)
{
    if (r == NULL || words == NULL || r->n_inputs == 0u || r->cap == 0u) {
        return 0u;
    }
    const uint32_t n_frames = n_words / r->n_inputs;
    uint32_t head = r->head;
    for (uint32_t f = 0; f < n_frames; ++f) {
        for (uint32_t in = 0; in < r->n_inputs; ++in) {
            r->ring[in][head] = words[f * r->n_inputs + in];
        }
        head = (head + 1u) % r->cap;
    }
    r->head = head;
    if (r->filled < r->cap) {
        r->filled = (r->filled + n_frames) < r->cap ? r->filled + n_frames : r->cap;
    }
    return n_frames;
}

void truing_audio_ring_tail(const truing_audio_ring_t *r, uint32_t input, uint32_t head, uint32_t n, int32_t *out)
{
    if (r == NULL || out == NULL || input >= r->n_inputs) {
        return;
    }
    const int32_t *ring = r->ring[input];
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = ring[(head + r->cap - n + i) % r->cap];
    }
}

void truing_audio_frames_extract(const int32_t *words, uint32_t n_frames, uint32_t n_inputs, uint32_t input,
                                 int32_t *out)
{
    if (words == NULL || out == NULL || input >= n_inputs) {
        return;
    }
    if (n_inputs == 1u) {
        memcpy(out, words, (size_t)n_frames * sizeof(int32_t));
        return;
    }
    for (uint32_t f = 0; f < n_frames; ++f) {
        out[f] = words[f * n_inputs + input];
    }
}
