#include "truing_dsp/spectrum.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MAG_EPS 1e-20f   /* log(0) guard only; not a tunable (spectrum.py _MAG_EPS) */

uint32_t truing_spectrum_n_fft(uint32_t n_samples, float zero_pad_factor)
{
    const double padded = ceil((double)zero_pad_factor * (double)n_samples);
    if (padded < 2.0) {
        return 2u;
    }
    if (padded > 4294967295.0) {
        return 0u;
    }
    return truing_dsp_next_pow2((uint32_t)padded);
}

static size_t align8(size_t v)
{
    return (v + 7u) & ~(size_t)7u;
}

size_t truing_spectrum_workspace_bytes(uint32_t n_fft_capacity, uint32_t max_window)
{
    const size_t n = n_fft_capacity;
    return align8(truing_fft_half_twiddle_bytes((uint32_t)(n / 2u))) +   /* half-angle twiddle */
           align8((n / 2u) * sizeof(truing_cpx_t)) +           /* scratch */
           align8(n * sizeof(float)) +                         /* windowed */
           align8((n / 2u + 1u) * sizeof(float)) +             /* log_mag */
           align8((size_t)max_window * sizeof(float));         /* hann */
}

bool truing_spectrum_workspace_init(truing_spectrum_workspace_t *ws, uint32_t n_fft_capacity, uint32_t max_window,
                                    void *block, size_t block_bytes)
{
    if (ws == NULL || block == NULL || n_fft_capacity < 8u || (n_fft_capacity & (n_fft_capacity - 1u)) != 0u ||
        max_window == 0u || block_bytes < truing_spectrum_workspace_bytes(n_fft_capacity, max_window)) {
        return false;
    }
    memset(ws, 0, sizeof(*ws));
    uint8_t *p = (uint8_t *)block;
    const size_t n = n_fft_capacity;
    ws->twiddle = (truing_cpx_t *)p;  p += align8(truing_fft_half_twiddle_bytes((uint32_t)(n / 2u)));
    ws->scratch = (truing_cpx_t *)p;  p += align8((n / 2u) * sizeof(truing_cpx_t));
    ws->windowed = (float *)p;        p += align8(n * sizeof(float));
    ws->log_mag = (float *)p;         p += align8((n / 2u + 1u) * sizeof(float));
    ws->hann = (float *)p;
    ws->max_window = max_window;
    ws->n_fft_capacity = n_fft_capacity;
    ws->plan.n = 0u;
    return true;
}

bool truing_spectrum_compute(truing_spectrum_workspace_t *ws, const float *x, uint32_t n, float sample_rate_hz,
                             float zero_pad_factor, truing_spectrum_t *out)
{
    if (ws == NULL || x == NULL || out == NULL || n < 4u) {
        return false;
    }
    const uint32_t n_fft = truing_spectrum_n_fft(n, zero_pad_factor);
    if (n_fft == 0u || n_fft > ws->n_fft_capacity || n_fft < n) {
        return false;
    }
    /* Periodic Hann (fftbins convention), computed in double as numpy does, applied in float. The
     * window depends only on its length, so it is built once and reused: on a target without a
     * double-precision FPU the cosines cost far more than the transform they feed. */
    if (n > ws->max_window) {
        return false;
    }
    if (ws->hann_n != n) {
        for (uint32_t k = 0; k < n; ++k) {
            ws->hann[k] = (float)(0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * (double)k / (double)n));
        }
        ws->hann_n = n;
    }
    for (uint32_t k = 0; k < n; ++k) {
        ws->windowed[k] = x[k] * ws->hann[k];
    }
    memset(ws->windowed + n, 0, (size_t)(n_fft - n) * sizeof(float));
    if (ws->plan.n != n_fft / 2u) {
        if (!truing_fft_plan_init_half(&ws->plan, n_fft / 2u, ws->twiddle)) {
            return false;
        }
    }
    truing_fft_real_log_magnitude(&ws->plan, ws->windowed, ws->scratch, ws->log_mag, MAG_EPS);
    const uint32_t n_bins = n_fft / 2u + 1u;
    out->log_mag_db = ws->log_mag;
    out->n_bins = n_bins;
    out->n_fft = n_fft;
    out->n_samples = n;
    out->sample_rate_hz = sample_rate_hz;
    out->bin_width_hz = sample_rate_hz / (float)n_fft;
    return true;
}

void truing_spectrum_parabolic(const float *y, uint32_t n, uint32_t k, float *delta_bins, float *peak_db)
{
    if (k == 0u || k + 1u >= n) {
        *delta_bins = 0.0f;
        *peak_db = y[k];
        return;
    }
    const float ym1 = y[k - 1u], y0 = y[k], yp1 = y[k + 1u];
    const float denom = ym1 - 2.0f * y0 + yp1;
    if (denom == 0.0f) {
        *delta_bins = 0.0f;
        *peak_db = y0;
        return;
    }
    float delta = 0.5f * (ym1 - yp1) / denom;
    if (!isfinite(delta)) {
        *delta_bins = 0.0f;
        *peak_db = y0;
        return;
    }
    if (delta > 0.5f) delta = 0.5f;
    if (delta < -0.5f) delta = -0.5f;
    *delta_bins = delta;
    *peak_db = y0 - 0.25f * (ym1 - yp1) * delta;
}

void truing_spectrum_refine_bin(const truing_spectrum_t *s, uint32_t k, float *freq_hz, float *mag_db)
{
    float delta, peak;
    truing_spectrum_parabolic(s->log_mag_db, s->n_bins, k, &delta, &peak);
    *freq_hz = ((float)k + delta) * s->bin_width_hz;
    *mag_db = peak;
}

bool truing_spectrum_argmax_in_band(const truing_spectrum_t *s, float lo_hz, float hi_hz, uint32_t *k_out)
{
    bool any = false;
    uint32_t best = 0u;
    float best_v = 0.0f;
    for (uint32_t k = 0; k < s->n_bins; ++k) {
        const float f = (float)k * s->bin_width_hz;
        if (f < lo_hz || f > hi_hz) {
            continue;
        }
        if (!any || s->log_mag_db[k] > best_v) {
            any = true;
            best = k;
            best_v = s->log_mag_db[k];
        }
    }
    if (any && k_out != NULL) {
        *k_out = best;
    }
    return any;
}
