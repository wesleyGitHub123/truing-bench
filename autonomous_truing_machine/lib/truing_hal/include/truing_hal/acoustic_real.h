/**
 * @file acoustic_real.h
 * The acoustic subsystem behind the one-call contract of acoustic_if.h (SPEC 9.1), composed
 * of the four seams (SPEC 9.2): a front end (audio_source_if), the ported layer 2, the
 * INTERIM layer 3 and the ported layer 4 (truing_dsp). Excitation, capture, onset
 * detection, analysis and model application are internal; the orchestrator sees a
 * TensionEstimate and nothing else.
 *
 * Status rules (SPEC 4.4.1, 7.4, 13.2):
 *   every successful estimate is `suspect` / PROVISIONAL_MODE_ID with mode identity
 *   presumed_fundamental, never `valid`, while layer 3 is interim;
 *   incomplete tension-model profile      -> unavailable / CALIBRATION_MISSING
 *   front end cannot deliver the format   -> init refuses; the interface answers unavailable / NOT_IMPLEMENTED
 *   capture timeout / hardware            -> unavailable / SENSOR_TIMEOUT
 *   dropped samples                       -> rejected    / CAPTURE_OVERRUN
 *   cancelled                             -> unavailable / CANCELLED
 *   no onset in the capture               -> rejected    / NO_ONSET_DETECTED
 *   too little signal after gating        -> rejected    / VALUE_OUT_OF_RANGE
 *   no strong peak at all                 -> rejected    / LOW_SNR
 *   strong peaks but none in the f1 band  -> rejected    / FREQ_OUT_OF_RANGE
 *   SNR below the chain's measurement gate-> rejected    / LOW_SNR
 *   model refuses                         -> rejected    / MODEL_REJECTED or NO_F2_PARTNER
 */
#ifndef TRUING_HAL_ACOUSTIC_REAL_H
#define TRUING_HAL_ACOUSTIC_REAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing_dsp/analysis.h"
#include "truing_dsp/onset.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/pluck_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostics of the last measurement, for telemetry and bring-up (not part of the record). */
typedef struct {
    truing_audio_result_t capture_result;
    uint32_t n_captured;
    uint32_t onsets;
    uint32_t onset_sample;
    float    onset_threshold;
    truing_dsp_window_t window;
    uint32_t n_fft;
    uint32_t n_strong_peaks;
    uint32_t n_peaks_in_band;
    float    f1_hz, f2_hz, snr_db;
    float    l_eff_m;
    uint32_t capture_us, analysis_us;   /* measured by the clock if it has that resolution */
    bool     pluck_commanded;
} truing_acoustic_real_diag_t;

typedef struct {
    truing_clock_if_t                  clock;
    const truing_chain_profile_t      *chain;
    const truing_tension_model_profile_t *profile;   /* session-fixed (SPEC 12.3.1) */
    truing_audio_source_if_t          *source;
    truing_pluck_if_t                 *pluck;        /* NULL: no actuator; the capture still runs */
    truing_dsp_params_t                params;
    truing_dsp_workspace_t             dsp;
    int32_t                           *words;        /* capture buffer, n_capture words */
    float                             *samples;      /* float view, n_capture */
    float                             *onset_env;    /* onset frame envelope scratch */
    uint32_t                           n_capture;
    uint32_t                           onset_env_cap;
    volatile bool                      cancel_requested;
    bool                               source_open;
    bool                               profile_ok;
    truing_acoustic_real_diag_t        diag;
    uint32_t                           calls;
    uint32_t                           estimates;
    uint32_t                           rejections;
} truing_acoustic_real_ctx_t;

/* Bytes of scratch the subsystem needs for this chain profile (capture buffers + DSP workspace). */
size_t truing_acoustic_real_scratch_bytes(const truing_chain_profile_t *chain);

/* Wire the seams. `scratch` must hold scratch_bytes(chain); it may live in PSRAM. Returns false
 * (and leaves the interface answering `unavailable`) when the chain profile is invalid, the
 * source refuses the system format, or the scratch is too small. An incomplete tension-model
 * profile does NOT fail init: every measurement then reports CALIBRATION_MISSING (SPEC 11.3.1). */
bool truing_acoustic_real_init(truing_acoustic_if_t *self, truing_acoustic_real_ctx_t *ctx, truing_clock_if_t clock,
                               const truing_chain_profile_t *chain, const truing_tension_model_profile_t *profile,
                               truing_audio_source_if_t *source, truing_pluck_if_t *pluck, void *scratch, size_t scratch_bytes,
                               const char **detail);
/* Analyse an already-captured buffer (n words) with no excitation: the path the bring-up and the
 * golden tests use. Identical to measure() after the capture step. */
void truing_acoustic_real_analyze_words(truing_acoustic_if_t *self, const int32_t *words, uint32_t n_words,
                                        uint8_t cycle_index, truing_tension_estimate_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_ACOUSTIC_REAL_H */
