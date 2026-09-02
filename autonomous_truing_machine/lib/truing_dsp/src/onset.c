#include "truing_dsp/onset.h"

#include <math.h>
#include <string.h>

static uint32_t round_samples(float ms, float fs)
{
    const float v = ms * 1e-3f * fs;
    const uint32_t r = (uint32_t)(v + 0.5f);
    return r == 0u ? 1u : r;
}

bool truing_onset_detect(const float *x, uint32_t n, const truing_dsp_params_t *p, float *env_scratch,
                         uint32_t env_cap, truing_onset_result_t *out)
{
    if (x == NULL || p == NULL || env_scratch == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->threshold = NAN;
    const float fs = p->sample_rate_hz;
    const uint32_t frame_n = round_samples(p->onset_frame_ms, fs);
    const uint32_t hop_n = round_samples(p->onset_hop_ms, fs);
    if (n < frame_n) {
        return true;   /* zero frames: no onsets, not an error */
    }
    const uint32_t n_frames = 1u + (n - frame_n) / hop_n;
    if (n_frames > env_cap) {
        return false;
    }
    float env_max = 0.0f;
    for (uint32_t i = 0; i < n_frames; ++i) {
        const float *f = x + (size_t)i * hop_n;
        double acc = 0.0;
        for (uint32_t k = 0; k < frame_n; ++k) {
            acc += (double)f[k] * (double)f[k];
        }
        env_scratch[i] = (float)sqrt(acc / (double)frame_n);
        if (env_scratch[i] > env_max) env_max = env_scratch[i];
    }
    out->n_frames = n_frames;
    out->envelope_max = env_max;
    if (env_max <= 0.0f) {
        /* Deliberate deviation from onset.py, which fires at threshold 0 on an all-zero input it never
         * sees: a capture with no energy has no onset. Every other input follows the reference. */
        out->threshold = 0.0f;
        return true;
    }
    float threshold = p->onset_threshold_rel * env_max;
    if (p->onset_threshold_abs > 0.0f && p->onset_threshold_abs > threshold) {
        threshold = p->onset_threshold_abs;
    }
    out->threshold = threshold;
    const uint32_t refractory_n = (uint32_t)(p->onset_refractory_s * fs + 0.5f);
    bool armed = true;
    int64_t block_until = -1;
    for (uint32_t i = 0; i < n_frames; ++i) {
        const uint32_t s = i * hop_n;
        if ((int64_t)s < block_until) {
            continue;
        }
        if (!armed) {
            if (env_scratch[i] < threshold) armed = true;
            continue;
        }
        if (env_scratch[i] >= threshold) {
            const uint32_t end = s + refractory_n;
            float strength = env_scratch[i];
            for (uint32_t j = i; j < n_frames && (j * hop_n) < end; ++j) {
                if (env_scratch[j] > strength) strength = env_scratch[j];
            }
            if (out->n_onsets < TRUING_ONSET_MAX) {
                out->onset_sample[out->n_onsets] = s;
                out->strength[out->n_onsets] = strength;
                out->n_onsets++;
            } else {
                out->overflow = true;
            }
            block_until = (int64_t)end;
            armed = false;
        }
    }
    return true;
}
