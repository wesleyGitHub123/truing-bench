#include "truing_dsp/analysis.h"

#include <math.h>
#include <string.h>

static size_t align8(size_t v)
{
    return (v + 7u) & ~(size_t)7u;
}

bool truing_dsp_params_from_chain(const truing_chain_profile_t *c, truing_dsp_params_t *out)
{
    if (c == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sample_rate_hz = (float)c->sample_rate_hz;
    out->onset_frame_ms = c->onset_frame_ms;
    out->onset_hop_ms = c->onset_hop_ms;
    out->onset_threshold_rel = c->onset_threshold_rel;
    out->onset_threshold_abs = c->onset_threshold_abs;
    out->onset_refractory_s = c->onset_refractory_s;
    out->gate_start_ms = c->gate_start_ms;
    out->window_ms = c->window_ms;
    out->decay_floor_db = c->decay_floor_db;
    out->decay_smoothing_ms = c->decay_smoothing_ms;
    out->next_onset_margin_ms = c->next_onset_margin_ms;
    out->min_window_ms = c->min_window_ms;
    out->zero_pad_factor = c->zero_pad_factor;
    out->search_band_lo_hz = c->search_band_lo_hz;
    out->search_band_hi_hz = c->search_band_hi_hz;
    out->prominence_db = c->prominence_db;
    out->max_peak_depth_db = c->max_peak_depth_db;
    out->max_peaks = c->max_peaks;
    out->f1_band_lo_hz = c->f1_band_lo_hz;
    out->f1_band_hi_hz = c->f1_band_hi_hz;
    out->f2_ratio_lo = c->f2_ratio_lo;
    out->f2_ratio_hi = c->f2_ratio_hi;
    out->snr_noise_offset_lo_hz = c->snr_noise_offset_lo_hz;
    out->snr_noise_offset_hi_hz = c->snr_noise_offset_hi_hz;
    return out->sample_rate_hz > 0.0f && out->window_ms > 0.0f && out->zero_pad_factor >= 1.0f && out->max_peaks > 0u &&
           out->max_peaks <= TRUING_MAX_CANDIDATE_PEAKS;
}

size_t truing_dsp_workspace_bytes(uint32_t max_window, float zero_pad_factor)
{
    const uint32_t n_fft = truing_spectrum_n_fft(max_window, zero_pad_factor);
    if (n_fft == 0u) {
        return 0u;
    }
    return align8(truing_spectrum_workspace_bytes(n_fft, max_window)) + align8(truing_envelope_workspace_bytes(max_window)) +
           2u * align8((size_t)max_window * sizeof(float));
}

bool truing_dsp_workspace_init(truing_dsp_workspace_t *ws, uint32_t max_window, float zero_pad_factor, void *block, size_t block_bytes)
{
    if (ws == NULL || block == NULL || max_window < 4u) {
        return false;
    }
    const size_t need = truing_dsp_workspace_bytes(max_window, zero_pad_factor);
    if (need == 0u || block_bytes < need) {
        return false;
    }
    memset(ws, 0, sizeof(*ws));
    const uint32_t n_fft = truing_spectrum_n_fft(max_window, zero_pad_factor);
    uint8_t *p = (uint8_t *)block;
    const size_t sb = align8(truing_spectrum_workspace_bytes(n_fft, max_window));
    if (!truing_spectrum_workspace_init(&ws->spectrum, n_fft, max_window, p, sb)) {
        return false;
    }
    p += sb;
    const size_t eb = align8(truing_envelope_workspace_bytes(max_window));
    if (!truing_envelope_workspace_init(&ws->envelope, max_window, p, eb)) {
        return false;
    }
    p += eb;
    ws->env = (float *)p;
    p += align8((size_t)max_window * sizeof(float));
    ws->env_smooth = (float *)p;
    ws->max_window = max_window;
    return true;
}

static uint32_t round_ms(float ms, float fs)
{
    return (uint32_t)(ms * 1e-3f * fs + 0.5f);
}

bool truing_dsp_select_window(truing_dsp_workspace_t *ws, const float *x, uint32_t n, int64_t onset_sample, int64_t next_onset,
                              const truing_dsp_params_t *p, truing_dsp_window_t *out)
{
    if (ws == NULL || x == NULL || p == NULL || out == NULL || onset_sample < 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const float fs = p->sample_rate_hz;
    const int64_t start = onset_sample + (int64_t)round_ms(p->gate_start_ms, fs);
    if (start >= (int64_t)n) {
        return false;
    }
    int64_t end = start + (int64_t)round_ms(p->window_ms, fs);
    truing_window_truncation_t by = TRUING_WINDOW_TRUNCATED_BY_WINDOW_MS;
    if (next_onset >= 0) {
        const int64_t limit = next_onset - (int64_t)round_ms(p->next_onset_margin_ms, fs);
        if (limit < end) {
            end = limit;
            by = TRUING_WINDOW_TRUNCATED_BY_NEXT_ONSET;
        }
    }
    if (end > (int64_t)n) {
        end = (int64_t)n;
        by = TRUING_WINDOW_TRUNCATED_BY_EOF;
    }
    if (end - start < 4) {
        return false;
    }
    uint32_t len = (uint32_t)(end - start);
    if (len > ws->max_window) {
        return false;   /* configuration asks for more than the workspace was sized for */
    }
    /* Decay-floor truncation judged on the smoothed analytic envelope (A10); samples untouched. */
    if (truing_envelope_analytic(&ws->envelope, x + start, len, ws->env)) {
        float peak = 0.0f;
        uint32_t peak_idx = 0u;
        bool any = false;
        for (uint32_t i = 0; i < len; ++i) {
            if (isfinite(ws->env[i]) && (!any || ws->env[i] > peak)) {
                peak = ws->env[i];
                peak_idx = i;
                any = true;
            }
        }
        if (any && peak > 0.0f) {
            const float floor_v = peak * powf(10.0f, -fabsf(p->decay_floor_db) / 20.0f);
            uint32_t width = round_ms(p->decay_smoothing_ms, fs);
            if (p->decay_smoothing_ms <= 0.0f) width = 1u;
            truing_envelope_smooth(ws->env, len, width, ws->env_smooth);
            for (uint32_t i = peak_idx; i < len; ++i) {
                if (ws->env_smooth[i] < floor_v) {
                    if (i >= 4u && i < len) {
                        len = i;
                        by = TRUING_WINDOW_TRUNCATED_BY_DECAY_FLOOR;
                    }
                    break;
                }
            }
        }
    }
    const float window_len_ms = 1e3f * (float)len / fs;
    if (window_len_ms < p->min_window_ms || len < 4u) {
        return false;
    }
    out->start_sample = (uint32_t)start;
    out->n_samples = len;
    out->window_len_ms = window_len_ms;
    out->truncated_by = by;
    return true;
}

bool truing_dsp_analyze_window(truing_dsp_workspace_t *ws, const float *x, const truing_dsp_window_t *w,
                               const truing_dsp_params_t *p, truing_dsp_event_t *out)
{
    if (ws == NULL || x == NULL || w == NULL || p == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->window = *w;
    out->snr_db = NAN;
    out->peak_spacing_hz = NAN;
    truing_spectrum_t spec;
    if (!truing_spectrum_compute(&ws->spectrum, x + w->start_sample, w->n_samples, p->sample_rate_hz, p->zero_pad_factor, &spec)) {
        return false;
    }
    out->n_fft = spec.n_fft;
    truing_peak_t strong[TRUING_DSP_MAX_STRONG_PEAKS];
    const uint32_t n_strong = truing_peaks_find(&spec, p->search_band_lo_hz, p->search_band_hi_hz, p->prominence_db,
                                                p->max_peak_depth_db, strong, TRUING_DSP_MAX_STRONG_PEAKS, &out->peaks_overflow);
    out->n_strong_peaks = n_strong;
    const uint32_t max_c = p->max_peaks < TRUING_MAX_CANDIDATE_PEAKS ? p->max_peaks : TRUING_MAX_CANDIDATE_PEAKS;
    out->n_candidates = (uint8_t)truing_peaks_top_by_prominence(strong, n_strong, max_c, out->candidates);
    out->f1_found = truing_peaks_identify_f1(strong, n_strong, p->f1_band_lo_hz, p->f1_band_hi_hz, &out->f1);
    if (out->f1_found) {
        out->f2_found = truing_peaks_identify_f2(strong, n_strong, out->f1.freq_hz, p->f2_ratio_lo, p->f2_ratio_hi, &out->f2);
        /* the spectrum's zero-padded input buffer is free once the transform is done: median scratch */
        out->snr_db = truing_peaks_snr_db(&spec, out->f1.freq_hz, out->f1.magnitude_db, p->snr_noise_offset_lo_hz,
                                          p->snr_noise_offset_hi_hz, ws->spectrum.windowed, spec.n_fft);
    }
    out->n_peaks_in_band = truing_peaks_count_in_band(strong, n_strong, p->f1_band_lo_hz, p->f1_band_hi_hz);
    out->peak_spacing_hz = truing_peaks_spacing_hz(strong, n_strong, p->f1_band_lo_hz, p->f1_band_hi_hz);
    return true;
}

const char *truing_window_truncation_str(truing_window_truncation_t t)
{
    switch (t) {
    case TRUING_WINDOW_TRUNCATED_BY_WINDOW_MS: return "window_ms";
    case TRUING_WINDOW_TRUNCATED_BY_DECAY_FLOOR: return "decay_floor";
    case TRUING_WINDOW_TRUNCATED_BY_NEXT_ONSET: return "next_onset";
    case TRUING_WINDOW_TRUNCATED_BY_EOF: return "eof";
    default: return "none";
    }
}
