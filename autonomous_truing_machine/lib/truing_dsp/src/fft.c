#include "truing_dsp/fft.h"

#include <math.h>

uint32_t truing_dsp_next_pow2(uint32_t n)
{
    if (n <= 1u) {
        return 1u;
    }
    uint32_t p = 1u;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

size_t truing_fft_twiddle_bytes(uint32_t n)
{
    return (size_t)(n / 2u) * sizeof(truing_cpx_t);
}

bool truing_fft_plan_init(truing_fft_plan_t *plan, uint32_t n, truing_cpx_t *twiddle_storage)
{
    if (plan == NULL || twiddle_storage == NULL || n < 2u || (n & (n - 1u)) != 0u) {
        return false;
    }
    plan->n = n;
    plan->log2n = 0u;
    while ((1u << plan->log2n) < n) {
        plan->log2n++;
    }
    plan->twiddle = twiddle_storage;
    const double step = -2.0 * 3.14159265358979323846 / (double)n;
    for (uint32_t k = 0; k < n / 2u; ++k) {
        plan->twiddle[k].re = (float)cos(step * (double)k);
        plan->twiddle[k].im = (float)sin(step * (double)k);
    }
    return true;
}

static uint32_t bit_reverse(uint32_t v, uint32_t bits)
{
    uint32_t r = 0u;
    for (uint32_t i = 0; i < bits; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

void truing_fft_complex(const truing_fft_plan_t *plan, truing_cpx_t *x, bool inverse)
{
    const uint32_t n = plan->n;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = bit_reverse(i, plan->log2n);
        if (j > i) {
            const truing_cpx_t t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    for (uint32_t len = 2u; len <= n; len <<= 1) {
        const uint32_t half = len >> 1;
        const uint32_t tstep = n / len;
        for (uint32_t start = 0u; start < n; start += len) {
            for (uint32_t k = 0u; k < half; ++k) {
                truing_cpx_t w = plan->twiddle[k * tstep];
                if (inverse) {
                    w.im = -w.im;
                }
                truing_cpx_t *a = &x[start + k];
                truing_cpx_t *b = &x[start + k + half];
                const float tr = b->re * w.re - b->im * w.im;
                const float ti = b->re * w.im + b->im * w.re;
                b->re = a->re - tr;
                b->im = a->im - ti;
                a->re += tr;
                a->im += ti;
            }
        }
    }
}

void truing_fft_real(const truing_fft_plan_t *half_plan, const float *x, truing_cpx_t *scratch, truing_cpx_t *out)
{
    const uint32_t n = half_plan->n;         /* complex length; real length is 2n */
    for (uint32_t k = 0; k < n; ++k) {
        scratch[k].re = x[2u * k];
        scratch[k].im = x[2u * k + 1u];
    }
    truing_fft_complex(half_plan, scratch, false);
    /* Split: X[k] = E[k] + W^k O[k], E = (Z[k] + conj Z[n-k])/2, O = -i (Z[k] - conj Z[n-k])/2. */
    const double step = -3.14159265358979323846 / (double)n;   /* exp(-i*pi*k/n) for the 2n-point transform */
    for (uint32_t k = 0; k <= n; ++k) {
        const truing_cpx_t zk = scratch[k == n ? 0u : k];
        const truing_cpx_t zc = scratch[(n - k) == n ? 0u : (n - k)];
        const float er = 0.5f * (zk.re + zc.re), ei = 0.5f * (zk.im - zc.im);
        const float or_ = 0.5f * (zk.im + zc.im), oi = -0.5f * (zk.re - zc.re);
        const float wr = (float)cos(step * (double)k), wi = (float)sin(step * (double)k);
        out[k].re = er + (or_ * wr - oi * wi);
        out[k].im = ei + (or_ * wi + oi * wr);
    }
}
