/**
 * @file audio_ring.h
 * The front end's pre-trigger ring and its input split, as pure C so the host tests exercise the
 * exact code the I2S drain task runs (SPEC 9.4). No allocation, no locking, no hardware.
 *
 * An I2S read delivers FRAMES: one word per input, interleaved in slot order (input 0 = the left
 * slot, input 1 = the right slot). With one input a frame is one word and the ring is the stream
 * as delivered. With two inputs each input gets its own ring of mono samples at the same indices,
 * so sample i of every input is the same instant -- which is what lets a second microphone serve
 * as a simultaneous reference without ever entering the first one's analysis.
 */
#ifndef TRUING_HAL_AUDIO_RING_H
#define TRUING_HAL_AUDIO_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_AUDIO_MAX_INPUTS 2u

typedef struct {
    int32_t          *ring[TRUING_AUDIO_MAX_INPUTS];   /* one per input, cap samples each */
    uint32_t          cap;                             /* samples per input */
    uint32_t          n_inputs;                        /* 1 or 2 */
    volatile uint32_t head;                            /* next write index (single producer) */
    volatile uint32_t filled;                          /* <= cap */
} truing_audio_ring_t;

/* Appends n_words of interleaved frames (n_words / n_inputs frames; a trailing partial frame is
 * ignored) and returns the frame count appended. */
uint32_t truing_audio_ring_append(truing_audio_ring_t *r, const int32_t *words, uint32_t n_words);

/* Copies the n samples of `input` that end just before index `head` (a head sampled earlier, so
 * the producer moving on does not slide the tail). n must not exceed r->filled. */
void truing_audio_ring_tail(const truing_audio_ring_t *r, uint32_t input, uint32_t head, uint32_t n, int32_t *out);

/* Copies `input`'s samples from n_frames interleaved frames into out (mono). */
void truing_audio_frames_extract(const int32_t *words, uint32_t n_frames, uint32_t n_inputs, uint32_t input,
                                 int32_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_AUDIO_RING_H */
