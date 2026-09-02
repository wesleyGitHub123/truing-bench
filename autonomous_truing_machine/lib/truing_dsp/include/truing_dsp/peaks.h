/**
 * @file peaks.h
 * Layer 2, part 2 (research repo lib/dsp/peaks.py): strong-peak candidates on the
 * log-magnitude spectrum with scipy.signal.find_peaks prominence semantics, refined
 * by parabolic interpolation; plus the INTERIM layer-3 rule (S3.5) and the S3.8 SNR.
 * Peak selection never alters the spectrum (research constraint C2).
 */
#ifndef TRUING_DSP_PEAKS_H
#define TRUING_DSP_PEAKS_H

#include <stdbool.h>
#include <stdint.h>

#include "truing_dsp/spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    freq_hz;
    float    prominence_db;
    float    magnitude_db;
    uint32_t bin_index;
} truing_peak_t;

/* All strong peaks inside [lo, hi] Hz (prominence >= prominence_db on the FULL spectrum, then
 * within max_peak_depth_db of the strongest in-band peak when that gate is > 0), sorted by
 * frequency. Returns the count written; `*overflow` is set when more qualified than `cap`. */
uint32_t truing_peaks_find(const truing_spectrum_t *s, float lo_hz, float hi_hz, float prominence_db,
                           float max_peak_depth_db, truing_peak_t *out, uint32_t cap, bool *overflow);
/* Top `max_n` by prominence (ties by frequency), written to `out` sorted by frequency. */
uint32_t truing_peaks_top_by_prominence(const truing_peak_t *peaks, uint32_t n, uint32_t max_n, truing_peak_t *out);
/* INTERIM layer 3 (S3.5): f1 = lowest strong peak inside the f1 band. False when none. */
bool truing_peaks_identify_f1(const truing_peak_t *peaks, uint32_t n, float lo_hz, float hi_hz, truing_peak_t *out);
/* f2 = most prominent peak inside [ratio_lo, ratio_hi] x f1. False when none. */
bool truing_peaks_identify_f2(const truing_peak_t *peaks, uint32_t n, float f1_hz, float ratio_lo, float ratio_hi, truing_peak_t *out);
uint32_t truing_peaks_count_in_band(const truing_peak_t *peaks, uint32_t n, float lo_hz, float hi_hz);
/* Spacing of the two most prominent peaks in the band; NAN when fewer than two. */
float truing_peaks_spacing_hz(const truing_peak_t *peaks, uint32_t n, float lo_hz, float hi_hz);
/* S3.8: peak magnitude minus the MEDIAN magnitude over the annulus lo <= |f - f1| <= hi.
 * `scratch` must hold at least as many floats as bins fall in the annulus (n_bins is safe). */
float truing_peaks_snr_db(const truing_spectrum_t *s, float f1_hz, float peak_mag_db, float offset_lo_hz,
                          float offset_hi_hz, float *scratch, uint32_t scratch_cap);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_PEAKS_H */
