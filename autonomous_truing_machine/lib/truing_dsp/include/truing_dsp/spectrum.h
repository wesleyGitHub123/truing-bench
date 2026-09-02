/**
 * @file spectrum.h
 * Layer 2, part 1 (research repo lib/dsp/spectrum.py): periodic Hann window, zero-pad to
 * a power of two, FFT, log-magnitude, parabolic sub-bin refinement. This is the ENTIRE
 * permitted path from raw sample to frequency: nothing filters, denoises, normalises or
 * averages (research constraint C2).
 */
#ifndef TRUING_DSP_SPECTRUM_H
#define TRUING_DSP_SPECTRUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing_dsp/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float   *log_mag_db;      /* n_bins = n_fft/2 + 1 entries */
    uint32_t n_bins;
    uint32_t n_fft;
    uint32_t n_samples;       /* window length before padding */
    float    sample_rate_hz;
    float    bin_width_hz;
} truing_spectrum_t;

/* Scratch a spectrum of up to `max_window` samples at `zero_pad_factor` needs. */
typedef struct {
    truing_fft_plan_t plan;         /* complex length n_fft/2 */
    truing_cpx_t     *twiddle;      /* n_fft/4 entries */
    truing_cpx_t     *scratch;      /* n_fft/2 entries */
    truing_cpx_t     *bins;         /* n_fft/2 + 1 entries */
    float            *windowed;     /* n_fft floats (zero-padded input) */
    float            *log_mag;      /* n_fft/2 + 1 floats */
    float            *hann;         /* max_window floats: the window, cached for `hann_n` */
    uint32_t          hann_n;       /* length the window was built for; 0 = none */
    uint32_t          max_window;
    uint32_t          n_fft_capacity;
} truing_spectrum_workspace_t;

uint32_t truing_spectrum_n_fft(uint32_t n_samples, float zero_pad_factor);
size_t   truing_spectrum_workspace_bytes(uint32_t n_fft_capacity, uint32_t max_window);
/* Carve the workspace out of one caller-owned block of at least workspace_bytes. */
bool     truing_spectrum_workspace_init(truing_spectrum_workspace_t *ws, uint32_t n_fft_capacity, uint32_t max_window,
                                        void *block, size_t block_bytes);

/* Hann-window, zero-pad and transform x[0..n). False if n < 4 or the transform exceeds the workspace. */
bool truing_spectrum_compute(truing_spectrum_workspace_t *ws, const float *x, uint32_t n, float sample_rate_hz,
                             float zero_pad_factor, truing_spectrum_t *out);

/* Parabolic vertex through y[k-1], y[k], y[k+1] (dB); delta clamped to +-0.5 bins. */
void  truing_spectrum_parabolic(const float *y, uint32_t n, uint32_t k, float *delta_bins, float *peak_db);
void  truing_spectrum_refine_bin(const truing_spectrum_t *s, uint32_t k, float *freq_hz, float *mag_db);
/* Largest bin inside [lo, hi] Hz; false when the band holds no bin. */
bool  truing_spectrum_argmax_in_band(const truing_spectrum_t *s, float lo_hz, float hi_hz, uint32_t *k_out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_SPECTRUM_H */
