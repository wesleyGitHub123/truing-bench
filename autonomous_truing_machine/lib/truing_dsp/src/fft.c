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

size_t truing_fft_half_twiddle_bytes(uint32_t n)
{
    return (size_t)n * sizeof(truing_cpx_t);
}

static bool plan_init(truing_fft_plan_t *plan, uint32_t n, truing_cpx_t *twiddle_storage, uint32_t stride)
{
    if (plan == NULL || twiddle_storage == NULL || n < 2u || (n & (n - 1u)) != 0u) {
        return false;
    }
    plan->n = n;
    plan->log2n = 0u;
    while ((1u << plan->log2n) < n) {
        plan->log2n++;
    }
    plan->tw_stride = stride;
    plan->twiddle = twiddle_storage;
    /* stride 1: exp(-2 pi i k / n), n/2 entries. stride 2: exp(-2 pi i k / 2n), n entries.
     * Both denominators are powers of two, so step*(2k) and (2*step)*k round identically and the
     * strided reads reproduce the stride-1 table exactly. */
    const double step = -2.0 * 3.14159265358979323846 / ((double)n * (double)stride);
    const uint32_t count = stride == 1u ? n / 2u : n;
    for (uint32_t k = 0; k < count; ++k) {
        plan->twiddle[k].re = (float)cos(step * (double)k);
        plan->twiddle[k].im = (float)sin(step * (double)k);
    }
    return true;
}

bool truing_fft_plan_init(truing_fft_plan_t *plan, uint32_t n, truing_cpx_t *twiddle_storage)
{
    return plan_init(plan, n, twiddle_storage, 1u);
}

bool truing_fft_plan_init_half(truing_fft_plan_t *plan, uint32_t n, truing_cpx_t *twiddle_storage)
{
    return plan_init(plan, n, twiddle_storage, 2u);
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

/* One radix-2 pass over [from, to), which is a whole number of len-sized blocks. Split out
 * because the transform runs its passes in two different orders and the body is the hot loop
 * of the whole DSP: the direction is folded into a +-1 multiplier on the twiddle's imaginary
 * part, which is exact, and the table index is advanced rather than recomputed, both of which
 * keep the arithmetic identical while taking a branch and two integer multiplies out of it. */
static void fft_pass(truing_cpx_t *x, uint32_t from, uint32_t to, uint32_t len, uint32_t tstep,
                     const truing_cpx_t *tw, float sgn)
{
    const uint32_t half = len >> 1;
    for (uint32_t start = from; start < to; start += len) {
        truing_cpx_t *a = x + start;
        truing_cpx_t *b = a + half;
        uint32_t ti = 0u;
        for (uint32_t k = 0u; k < half; ++k) {
            const float wr = tw[ti].re;
            const float wi = sgn * tw[ti].im;
            ti += tstep;
            const float br = b->re, bi = b->im;
            const float tr = br * wr - bi * wi;
            const float tim = br * wi + bi * wr;
            b->re = a->re - tr;
            b->im = a->im - tim;
            a->re += tr;
            a->im += tim;
            ++a;
            ++b;
        }
    }
}

void truing_fft_complex(const truing_fft_plan_t *plan, truing_cpx_t *x, bool inverse)
{
    const uint32_t n = plan->n;
    const truing_cpx_t *tw = plan->twiddle;
    const uint32_t stride = plan->tw_stride;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = bit_reverse(i, plan->log2n);
        if (j > i) {
            const truing_cpx_t t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    const float sgn = inverse ? -1.0f : 1.0f;
    /* The butterflies of one pass are independent of each other, so the order in which a pass
     * sweeps its blocks is free to choose. While len <= block no butterfly reaches outside a
     * block-aligned chunk, so those passes are run chunk by chunk with the chunk resident in the
     * data cache, instead of streaming the whole array out of PSRAM once per pass. Every
     * butterfly, its two operands and its own order of operations are unchanged, so the output
     * is bit-identical to the linear order; only the misses differ.
     *
     * On the ESP32-S3 that is most of the cost. A transform small enough to sit in the 64 KB
     * data cache runs at 0.20 us per butterfly whether it is addressed in internal RAM or in
     * PSRAM, while the 131,072-point transform runs at 0.98 us: nearly four fifths of the time
     * is cache misses, not arithmetic. Blocking at 4096 complex elements (32 KB, leaving the
     * rest of the cache for the twiddle table) takes a pluck from 3,667 to 3,077 ms. It is a
     * local optimum on this part -- 2048 and 8192 both measured slower -- because the twiddles
     * of the larger blocked passes stride the whole table and start missing themselves. */
    const uint32_t block = n < (uint32_t)TRUING_FFT_CACHE_BLOCK ? n : (uint32_t)TRUING_FFT_CACHE_BLOCK;
    for (uint32_t c0 = 0u; c0 < n; c0 += block) {
        for (uint32_t len = 2u; len <= block; len <<= 1) {
            fft_pass(x, c0, c0 + block, len, (n / len) * stride, tw, sgn);
        }
    }
    for (uint32_t len = block << 1; len <= n; len <<= 1) {
        fft_pass(x, 0u, n, len, (n / len) * stride, tw, sgn);
    }
}

static void fft_real_core(const truing_fft_plan_t *half_plan, const float *x, truing_cpx_t *scratch,
                          truing_cpx_t *out, float *log_mag_out, float mag_eps)
{
    const uint32_t n = half_plan->n;         /* complex length; real length is 2n */
    for (uint32_t k = 0; k < n; ++k) {
        scratch[k].re = x[2u * k];
        scratch[k].im = x[2u * k + 1u];
    }
    truing_fft_complex(half_plan, scratch, false);
    /* Split: X[k] = E[k] + W^k O[k], E = (Z[k] + conj Z[n-k])/2, O = -i (Z[k] - conj Z[n-k])/2.
     * W^k = exp(-i*pi*k/n) is entry k of the half-angle table, so the 2n+2 double trigonometric
     * evaluations this loop used to perform per call are now a table read. Only k == n, which the
     * table does not hold, is still computed — once. */
    const double step = -3.14159265358979323846 / (double)n;
    for (uint32_t k = 0; k <= n; ++k) {
        const truing_cpx_t zk = scratch[k == n ? 0u : k];
        const truing_cpx_t zc = scratch[(n - k) == n ? 0u : (n - k)];
        const float er = 0.5f * (zk.re + zc.re), ei = 0.5f * (zk.im - zc.im);
        const float or_ = 0.5f * (zk.im + zc.im), oi = -0.5f * (zk.re - zc.re);
        float wr, wi;
        if (k < n) {
            wr = half_plan->twiddle[k].re;
            wi = half_plan->twiddle[k].im;
        } else {
            wr = (float)cos(step * (double)k);
            wi = (float)sin(step * (double)k);
        }
        const float xr = er + (or_ * wr - oi * wi);
        const float xi = ei + (or_ * wi + oi * wr);
        if (out != NULL) {
            out[k].re = xr;
            out[k].im = xi;
        } else {
            const float mag = sqrtf(xr * xr + xi * xi);
            log_mag_out[k] = 20.0f * log10f(mag > mag_eps ? mag : mag_eps);
        }
    }
}

void truing_fft_real(const truing_fft_plan_t *half_plan, const float *x, truing_cpx_t *scratch, truing_cpx_t *out)
{
    fft_real_core(half_plan, x, scratch, out, NULL, 0.0f);
}

void truing_fft_real_log_magnitude(const truing_fft_plan_t *half_plan, const float *x, truing_cpx_t *scratch,
                                   float *log_mag_out, float mag_eps)
{
    fft_real_core(half_plan, x, scratch, NULL, log_mag_out, mag_eps);
}
