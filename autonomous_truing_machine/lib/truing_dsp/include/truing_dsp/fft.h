/**
 * @file fft.h
 * Power-of-two complex FFT (iterative radix-2, float32) and a real-input transform
 * built on it. The caller owns every buffer (SPEC 9.4.1: analysis scratch may live in
 * PSRAM); nothing here allocates. Twiddles are computed in double at plan time so the
 * float32 transform's error is that of the arithmetic, not of the table.
 */
#ifndef TRUING_DSP_FFT_H
#define TRUING_DSP_FFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float re;
    float im;
} truing_cpx_t;

typedef struct {
    uint32_t     n;          /* complex transform length, power of two >= 2 */
    uint32_t     log2n;
    truing_cpx_t *twiddle;   /* n/2 entries: exp(-2*pi*i*k/n) */
} truing_fft_plan_t;

/* Bytes of twiddle storage a plan of complex length n needs. */
size_t truing_fft_twiddle_bytes(uint32_t n);
/* Build a plan over caller-owned twiddle storage. False if n is not a power of two >= 2. */
bool truing_fft_plan_init(truing_fft_plan_t *plan, uint32_t n, truing_cpx_t *twiddle_storage);
/* In-place complex FFT (inverse: conjugate twiddles, no 1/n scaling). */
void truing_fft_complex(const truing_fft_plan_t *plan, truing_cpx_t *x, bool inverse);
/* Real input of length 2n through an n-point complex plan: X[0..n] (n+1 bins) written to
 * `out`, which needs n+1 entries; `x` (2n floats) is read through `scratch` (n complex). */
void truing_fft_real(const truing_fft_plan_t *half_plan, const float *x, truing_cpx_t *scratch, truing_cpx_t *out);

uint32_t truing_dsp_next_pow2(uint32_t n);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_FFT_H */
