/**
 * @file onset.h
 * Event segmentation (research repo lib/dsp/onset.py, S3.1): frame-wise RMS envelope,
 * rising threshold crossing, refractory period. Threshold = max(rel * max(envelope),
 * abs) computed from THIS capture only. Its failure mode is silent, which is why the
 * subsystem reports NO_ONSET_DETECTED rather than analysing whatever it finds.
 */
#ifndef TRUING_DSP_ONSET_H
#define TRUING_DSP_ONSET_H

#include <stdbool.h>
#include <stdint.h>

#include "truing_dsp/params.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_ONSET_MAX 16u

typedef struct {
    uint32_t n_onsets;
    uint32_t onset_sample[TRUING_ONSET_MAX];
    float    strength[TRUING_ONSET_MAX];    /* peak envelope within the refractory span */
    float    threshold;                     /* absolute RMS value used (NAN when no frames) */
    float    envelope_max;
    uint32_t n_frames;
    bool     overflow;                      /* more than TRUING_ONSET_MAX onsets fired */
} truing_onset_result_t;

/* `env_scratch` holds the frame envelope: at least 1 + (n - frame)/hop floats. */
bool truing_onset_detect(const float *x, uint32_t n, const truing_dsp_params_t *p, float *env_scratch,
                         uint32_t env_cap, truing_onset_result_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DSP_ONSET_H */
