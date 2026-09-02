#include "truing_dsp/envelope.h"

#include <math.h>
#include <string.h>

static size_t align8(size_t v)
{
    return (v + 7u) & ~(size_t)7u;
}

static uint32_t bluestein_m(uint32_t max_n)
{
    return truing_dsp_next_pow2(2u * max_n - 1u);
}

size_t truing_envelope_workspace_bytes(uint32_t max_n)
{
    const size_t m = bluestein_m(max_n);
    return align8((m / 2u) * sizeof(truing_cpx_t)) + 2u * align8(m * sizeof(truing_cpx_t)) +
           2u * align8((size_t)max_n * sizeof(truing_cpx_t));
}

bool truing_envelope_workspace_init(truing_envelope_workspace_t *ws, uint32_t max_n, void *block, size_t block_bytes)
{
    if (ws == NULL || block == NULL || max_n < 2u || block_bytes < truing_envelope_workspace_bytes(max_n)) {
        return false;
    }
    memset(ws, 0, sizeof(*ws));
    const uint32_t m = bluestein_m(max_n);
    uint8_t *p = (uint8_t *)block;
    ws->twiddle = (truing_cpx_t *)p;  p += align8((m / 2u) * sizeof(truing_cpx_t));
    ws->a = (truing_cpx_t *)p;        p += align8(m * sizeof(truing_cpx_t));
    ws->b = (truing_cpx_t *)p;        p += align8(m * sizeof(truing_cpx_t));
    ws->chirp = (truing_cpx_t *)p;    p += align8((size_t)max_n * sizeof(truing_cpx_t));
    ws->spectrum = (truing_cpx_t *)p;
    ws->max_n = max_n;
    ws->m = m;
    return truing_fft_plan_init(&ws->plan, m, ws->twiddle);
}

bool truing_dsp_dft(truing_envelope_workspace_t *ws, const truing_cpx_t *x, uint32_t n, bool inverse, truing_cpx_t *out)
{
    if (ws == NULL || x == NULL || out == NULL || n < 1u || n > ws->max_n) {
        return false;
    }
    const uint32_t m = ws->m;
    /* chirp w_k = exp(-i*pi*k^2/n); k^2 reduced mod 2n keeps the double argument small. */
    const double sgn = inverse ? 1.0 : -1.0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint64_t k2 = ((uint64_t)k * (uint64_t)k) % (2ull * (uint64_t)n);
        const double ang = sgn * 3.14159265358979323846 * (double)k2 / (double)n;
        ws->chirp[k].re = (float)cos(ang);
        ws->chirp[k].im = (float)sin(ang);
    }
    memset(ws->a, 0, (size_t)m * sizeof(truing_cpx_t));
    memset(ws->b, 0, (size_t)m * sizeof(truing_cpx_t));
    for (uint32_t k = 0; k < n; ++k) {
        /* a_k = x_k * w_k ; b_k = conj(w_k), b_{m-k} = conj(w_k) */
        const truing_cpx_t w = ws->chirp[k];
        ws->a[k].re = x[k].re * w.re - x[k].im * w.im;
        ws->a[k].im = x[k].re * w.im + x[k].im * w.re;
        ws->b[k].re = w.re;
        ws->b[k].im = -w.im;
        if (k > 0u) {
            ws->b[m - k].re = w.re;
            ws->b[m - k].im = -w.im;
        }
    }
    truing_fft_complex(&ws->plan, ws->a, false);
    truing_fft_complex(&ws->plan, ws->b, false);
    for (uint32_t k = 0; k < m; ++k) {
        const float re = ws->a[k].re * ws->b[k].re - ws->a[k].im * ws->b[k].im;
        const float im = ws->a[k].re * ws->b[k].im + ws->a[k].im * ws->b[k].re;
        ws->a[k].re = re;
        ws->a[k].im = im;
    }
    truing_fft_complex(&ws->plan, ws->a, true);
    const float inv_m = 1.0f / (float)m;
    for (uint32_t k = 0; k < n; ++k) {
        const truing_cpx_t w = ws->chirp[k];
        const float re = ws->a[k].re * inv_m, im = ws->a[k].im * inv_m;
        out[k].re = re * w.re - im * w.im;
        out[k].im = re * w.im + im * w.re;
    }
    return true;
}

bool truing_envelope_analytic(truing_envelope_workspace_t *ws, const float *x, uint32_t n, float *env)
{
    if (ws == NULL || x == NULL || env == NULL || n < 1u || n > ws->max_n) {
        return false;
    }
    /* scipy.signal.hilbert: X = fft(x); h = [1, 2...2, (1 if n even), 0...0]; x_a = ifft(X*h).
     * `spectrum` serves as input and output: truing_dsp_dft reads x completely into `a`
     * before it writes `out`, so the in-place call is safe. */
    truing_cpx_t *in = ws->spectrum;
    for (uint32_t k = 0; k < n; ++k) {
        in[k].re = x[k];
        in[k].im = 0.0f;
    }
    if (!truing_dsp_dft(ws, in, n, false, in)) {
        return false;
    }
    for (uint32_t k = 0; k < n; ++k) {
        float h;
        if (k == 0u) h = 1.0f;
        else if (2u * k < n) h = 2.0f;
        else if (2u * k == n) h = 1.0f;   /* even n: Nyquist kept once */
        else h = 0.0f;
        in[k].re *= h;
        in[k].im *= h;
    }
    if (!truing_dsp_dft(ws, in, n, true, in)) {
        return false;
    }
    const float inv_n = 1.0f / (float)n;
    for (uint32_t k = 0; k < n; ++k) {
        const float re = in[k].re * inv_n, im = in[k].im * inv_n;
        env[k] = sqrtf(re * re + im * im);
    }
    return true;
}

void truing_envelope_smooth(const float *env, uint32_t n, uint32_t width, float *out)
{
    if (env == NULL || out == NULL || n == 0u) {
        return;
    }
    if (width <= 1u) {
        memcpy(out, env, (size_t)n * sizeof(float));
        return;
    }
    /* scipy uniform_filter1d, origin 0: window [i - size//2, i - size//2 + size - 1], mode nearest. */
    const int64_t left = (int64_t)(width / 2u);
    const int64_t right = (int64_t)width - 1 - left;
    for (uint32_t i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int64_t j = (int64_t)i - left; j <= (int64_t)i + right; ++j) {
            const int64_t jj = j < 0 ? 0 : (j >= (int64_t)n ? (int64_t)n - 1 : j);
            acc += env[jj];
        }
        out[i] = (float)(acc / (double)width);
    }
}
