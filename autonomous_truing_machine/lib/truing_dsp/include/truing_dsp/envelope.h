/**
 * @file envelope.h
 * Analytic (Hilbert) envelope and its moving-average smoothing, used ONLY to decide where
 * the analysis window ends (research S3.2 / A10). scipy.signal.hilbert transforms at the
 * segment's own length, which is not a power of two, so the port uses a Bluestein chirp-z
 * transform over power-of-two FFTs to reproduce it exactly rather than zero-padding.
 */
#ifndef TRUING_DSP_ENVELOPE_H
#define TRUING_DSP_ENVELOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing_dsp/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    truing_fft_plan_t plan;      /* complex length M = next_pow2(2*max_n - 1) */
    truing_cpx_t     *twiddle;   /* M/2 */
    truing_cpx_t     *a;         /* M */
    truing_cpx_t     *b;         /* M */
    truing_cpx_t     *chirp;     /* max_n */
    truing_cpx_t     *spectrum;  /* max_n: the length-n DFT */
    uint32_t          max_n;
    uint32_t          m;
} truing_envelope_workspace_t;

size_t truing_envelope_workspace_bytes(uint32_t max_n);
bool   truing_envelope_workspace_init(truing_envelope_workspace_t *ws, uint32_t max_n, void *block, size_t block_bytes);

/* Arbitrary-length DFT of complex input x[0..n) into out[0..n) (Bluestein). inverse: no 1/n scaling. */
bool truing_dsp_dft(truing_envelope_workspace_t *ws, const truing_cpx_t *x, uint32_t n, bool inverse, truing_cpx_t *out);
/* |hilbert(x)| for real x[0..n), n <= max_n. `env` may not alias x. */
bool truing_envelope_analytic(truing_envelope_workspace_t *ws, const float *x, uint32_t n, float *env);
/* scipy.ndimage.uniform_filter1d(env, size=width, mode="nearest"); width <= 1 copies. `out` may not alias. */
void truing_envelope_smooth(const float *env, uint32_t n, uint32_t width, float *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_ENVELOPE_H */
