/**
 * @file params.h
 * Every DSP constant of the acoustic pipeline as one parameter block (SPEC 9.5: "DSP
 * window and gate constants are research outputs. Configuration, never literals").
 * Values arrive from the acoustic chain profile (SPEC 11.3); the research repository's
 * config/dsp.yaml is the reference for their meaning.
 */
#ifndef TRUING_DSP_PARAMS_H
#define TRUING_DSP_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    sample_rate_hz;
    /* onset (S3.1) */
    float    onset_frame_ms;
    float    onset_hop_ms;
    float    onset_threshold_rel;      /* fraction of the capture's peak RMS envelope */
    float    onset_threshold_abs;      /* absolute RMS floor (full scale = 1.0); <= 0 disables */
    float    onset_refractory_s;
    /* window gating (S3.2) */
    float    gate_start_ms;
    float    window_ms;
    float    decay_floor_db;
    float    decay_smoothing_ms;
    float    next_onset_margin_ms;
    float    min_window_ms;
    /* spectrum (S3.3) */
    float    zero_pad_factor;
    /* peaks (S3.4) */
    float    search_band_lo_hz;
    float    search_band_hi_hz;
    float    prominence_db;
    float    max_peak_depth_db;        /* <= 0 disables the relative-depth gate */
    uint8_t  max_peaks;                /* candidates reported (<= TRUING_MAX_CANDIDATE_PEAKS) */
    /* modes (S3.5, interim layer 3) */
    float    f1_band_lo_hz;
    float    f1_band_hi_hz;
    float    f2_ratio_lo;
    float    f2_ratio_hi;
    /* quality (S3.8) */
    float    snr_noise_offset_lo_hz;
    float    snr_noise_offset_hi_hz;
} truing_dsp_params_t;

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_PARAMS_H */
