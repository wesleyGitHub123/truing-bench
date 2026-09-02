/**
 * @file analysis.h
 * One pluck, raw samples to frequency candidates (research repo lib/dsp/features.py,
 * S3.2-S3.5, S3.8): gate -> window (Hann) -> zero-pad -> FFT -> peaks -> parabolic
 * interpolation, then the INTERIM layer-3 selection. The output is a SELECTED candidate
 * plus identity metadata; it never claims to have established the physical mode (SPEC 6.3).
 */
#ifndef TRUING_DSP_ANALYSIS_H
#define TRUING_DSP_ANALYSIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing/limits.h"
#include "truing_dsp/envelope.h"
#include "truing_dsp/params.h"
#include "truing_dsp/peaks.h"
#include "truing_dsp/spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_DSP_MAX_STRONG_PEAKS 128u
#define TRUING_DSP_SELECTION_RULE_VERSION 1u   /* "lowest strong peak in the f1 band" (research repo current rule) */

typedef enum {
    TRUING_WINDOW_TRUNCATED_BY_NONE = 0,
    TRUING_WINDOW_TRUNCATED_BY_WINDOW_MS,
    TRUING_WINDOW_TRUNCATED_BY_DECAY_FLOOR,
    TRUING_WINDOW_TRUNCATED_BY_NEXT_ONSET,
    TRUING_WINDOW_TRUNCATED_BY_EOF,
} truing_window_truncation_t;

typedef struct {
    uint32_t start_sample;
    uint32_t n_samples;
    float    window_len_ms;
    truing_window_truncation_t truncated_by;
} truing_dsp_window_t;

typedef struct {
    truing_spectrum_workspace_t spectrum;
    truing_envelope_workspace_t envelope;
    float   *env;            /* max_window floats */
    float   *env_smooth;     /* max_window floats */
    uint32_t max_window;
} truing_dsp_workspace_t;

typedef struct {
    truing_dsp_window_t window;
    uint32_t n_fft;
    bool     f1_found;
    truing_peak_t f1;
    bool     f2_found;
    truing_peak_t f2;
    float    snr_db;
    uint32_t n_strong_peaks;
    uint32_t n_peaks_in_band;
    bool     peaks_overflow;
    float    peak_spacing_hz;                       /* NAN when < 2 in band */
    uint8_t  n_candidates;
    truing_peak_t candidates[TRUING_MAX_CANDIDATE_PEAKS];   /* top by prominence, sorted by frequency */
} truing_dsp_event_t;

/* Chain profile (SPEC 11.3) -> parameter block. False when the profile is unusable. */
bool truing_dsp_params_from_chain(const truing_chain_profile_t *chain, truing_dsp_params_t *out);

/* Workspace sizing for windows up to `max_window` samples at `zero_pad_factor`. */
size_t truing_dsp_workspace_bytes(uint32_t max_window, float zero_pad_factor);
bool   truing_dsp_workspace_init(truing_dsp_workspace_t *ws, uint32_t max_window, float zero_pad_factor, void *block, size_t block_bytes);

/* S3.2 gate and truncation. `next_onset` < 0 when there is none. False when too little signal survives. */
bool truing_dsp_select_window(truing_dsp_workspace_t *ws, const float *x, uint32_t n, int64_t onset_sample, int64_t next_onset,
                              const truing_dsp_params_t *p, truing_dsp_window_t *out);
/* Spectrum, candidates and the interim selection on x[window]. */
bool truing_dsp_analyze_window(truing_dsp_workspace_t *ws, const float *x, const truing_dsp_window_t *w,
                               const truing_dsp_params_t *p, truing_dsp_event_t *out);

const char *truing_window_truncation_str(truing_window_truncation_t t);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_ANALYSIS_H */
