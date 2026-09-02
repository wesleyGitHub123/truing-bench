#include "truing_dsp/peaks.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* scipy.signal._peak_finding_utils._peak_prominences with wlen unset: scan outward from the
 * peak while samples are <= the peak, taking the minimum on each side. */
static float prominence_at(const float *x, uint32_t n, uint32_t peak)
{
    const float v = x[peak];
    float left_min = v, right_min = v;
    for (uint32_t i = peak;; --i) {
        if (x[i] > v) break;
        if (x[i] < left_min) left_min = x[i];
        if (i == 0u) break;
    }
    for (uint32_t i = peak; i < n; ++i) {
        if (x[i] > v) break;
        if (x[i] < right_min) right_min = x[i];
    }
    return v - (left_min > right_min ? left_min : right_min);
}

static int cmp_freq(const void *a, const void *b)
{
    const float fa = ((const truing_peak_t *)a)->freq_hz, fb = ((const truing_peak_t *)b)->freq_hz;
    return (fa > fb) - (fa < fb);
}

static int cmp_prom_desc_then_freq(const void *a, const void *b)
{
    const truing_peak_t *pa = (const truing_peak_t *)a, *pb = (const truing_peak_t *)b;
    if (pa->prominence_db != pb->prominence_db) {
        return (pa->prominence_db < pb->prominence_db) - (pa->prominence_db > pb->prominence_db);
    }
    return (pa->freq_hz > pb->freq_hz) - (pa->freq_hz < pb->freq_hz);
}

uint32_t truing_peaks_find(const truing_spectrum_t *s, float lo_hz, float hi_hz, float prominence_db,
                           float max_peak_depth_db, truing_peak_t *out, uint32_t cap, bool *overflow)
{
    if (overflow != NULL) {
        *overflow = false;
    }
    if (s == NULL || out == NULL || cap == 0u || s->n_bins < 3u) {
        return 0u;
    }
    const float *x = s->log_mag_db;
    const uint32_t n = s->n_bins;
    uint32_t count = 0u;
    float band_max = -INFINITY;
    /* scipy.signal._peak_finding_utils._local_maxima_1d: strict rise, plateau midpoint. */
    const uint32_t i_max = n - 1u;
    uint32_t i = 1u;
    while (i < i_max) {
        if (x[i - 1u] < x[i]) {
            uint32_t ahead = i + 1u;
            while (ahead < i_max && x[ahead] == x[i]) {
                ahead++;
            }
            if (x[ahead] < x[i]) {
                const uint32_t mid = (i + ahead - 1u) / 2u;
                const float f_bin = (float)mid * s->bin_width_hz;      /* band test on the BIN frequency (peaks.py) */
                if (f_bin >= lo_hz && f_bin <= hi_hz) {
                    const float prom = prominence_at(x, n, mid);
                    if (prom >= prominence_db) {
                        truing_peak_t cand;
                        cand.bin_index = mid;
                        cand.prominence_db = prom;
                        truing_spectrum_refine_bin(s, mid, &cand.freq_hz, &cand.magnitude_db);
                        /* The depth gate (peaks.py: magnitude >= band max - depth) is applied against a
                         * RUNNING band maximum: a peak below the current floor can never qualify because
                         * the maximum only rises, and raising the maximum prunes what no longer qualifies.
                         * The result equals the reference's collect-then-filter with bounded storage. */
                        const bool gated = max_peak_depth_db > 0.0f;
                        if (!gated || count == 0u || cand.magnitude_db >= band_max - max_peak_depth_db) {
                            if (cand.magnitude_db > band_max) {
                                band_max = cand.magnitude_db;
                                if (gated) {
                                    const float floor_db = band_max - max_peak_depth_db;
                                    uint32_t kept = 0u;
                                    for (uint32_t k = 0; k < count; ++k) {
                                        if (out[k].magnitude_db >= floor_db) out[kept++] = out[k];
                                    }
                                    count = kept;
                                }
                            }
                            if (count < cap) {
                                out[count++] = cand;
                            } else if (overflow != NULL) {
                                *overflow = true;
                            }
                        }
                    }
                }
                i = ahead;   /* scipy skips the plateau only when a peak was recorded */
            }
        }
        i++;
    }
    qsort(out, count, sizeof(truing_peak_t), cmp_freq);
    return count;
}

uint32_t truing_peaks_top_by_prominence(const truing_peak_t *peaks, uint32_t n, uint32_t max_n, truing_peak_t *out)
{
    if (peaks == NULL || out == NULL || n == 0u || max_n == 0u) {
        return 0u;
    }
    /* Rank without disturbing the caller's list: selection of the top max_n. */
    truing_peak_t ranked[128];
    if (n > 128u) n = 128u;
    memcpy(ranked, peaks, n * sizeof(truing_peak_t));
    qsort(ranked, n, sizeof(truing_peak_t), cmp_prom_desc_then_freq);
    const uint32_t m = n < max_n ? n : max_n;
    memcpy(out, ranked, m * sizeof(truing_peak_t));
    qsort(out, m, sizeof(truing_peak_t), cmp_freq);
    return m;
}

bool truing_peaks_identify_f1(const truing_peak_t *peaks, uint32_t n, float lo_hz, float hi_hz, truing_peak_t *out)
{
    /* peaks are sorted by frequency: the first in band is the lowest. */
    for (uint32_t i = 0; i < n; ++i) {
        if (peaks[i].freq_hz >= lo_hz && peaks[i].freq_hz <= hi_hz) {
            if (out != NULL) *out = peaks[i];
            return true;
        }
    }
    return false;
}

bool truing_peaks_identify_f2(const truing_peak_t *peaks, uint32_t n, float f1_hz, float ratio_lo, float ratio_hi, truing_peak_t *out)
{
    if (!isfinite(f1_hz) || f1_hz <= 0.0f) {
        return false;
    }
    const float lo = ratio_lo * f1_hz, hi = ratio_hi * f1_hz;
    bool found = false;
    truing_peak_t best;
    memset(&best, 0, sizeof(best));
    for (uint32_t i = 0; i < n; ++i) {
        const truing_peak_t *p = &peaks[i];
        if (p->freq_hz < lo || p->freq_hz > hi) continue;
        /* max by (prominence, -freq): higher prominence wins, ties go to the LOWER frequency */
        if (!found || p->prominence_db > best.prominence_db ||
            (p->prominence_db == best.prominence_db && p->freq_hz < best.freq_hz)) {
            best = *p;
            found = true;
        }
    }
    if (found && out != NULL) *out = best;
    return found;
}

uint32_t truing_peaks_count_in_band(const truing_peak_t *peaks, uint32_t n, float lo_hz, float hi_hz)
{
    uint32_t c = 0u;
    for (uint32_t i = 0; i < n; ++i) {
        if (peaks[i].freq_hz >= lo_hz && peaks[i].freq_hz <= hi_hz) c++;
    }
    return c;
}

float truing_peaks_spacing_hz(const truing_peak_t *peaks, uint32_t n, float lo_hz, float hi_hz)
{
    truing_peak_t in_band[128];
    uint32_t m = 0u;
    for (uint32_t i = 0; i < n && m < 128u; ++i) {
        if (peaks[i].freq_hz >= lo_hz && peaks[i].freq_hz <= hi_hz) in_band[m++] = peaks[i];
    }
    if (m < 2u) {
        return NAN;
    }
    qsort(in_band, m, sizeof(truing_peak_t), cmp_prom_desc_then_freq);
    return fabsf(in_band[0].freq_hz - in_band[1].freq_hz);
}

static int cmp_float(const void *a, const void *b)
{
    const float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

float truing_peaks_snr_db(const truing_spectrum_t *s, float f1_hz, float peak_mag_db, float offset_lo_hz,
                          float offset_hi_hz, float *scratch, uint32_t scratch_cap)
{
    if (s == NULL || scratch == NULL || !isfinite(f1_hz)) {
        return NAN;
    }
    uint32_t m = 0u;
    for (uint32_t k = 0; k < s->n_bins; ++k) {
        const float d = fabsf((float)k * s->bin_width_hz - f1_hz);
        if (d >= offset_lo_hz && d <= offset_hi_hz) {
            if (m >= scratch_cap) return NAN;
            scratch[m++] = s->log_mag_db[k];
        }
    }
    if (m == 0u) {
        return NAN;
    }
    qsort(scratch, m, sizeof(float), cmp_float);
    const float median = (m % 2u == 1u) ? scratch[m / 2u] : 0.5f * (scratch[m / 2u - 1u] + scratch[m / 2u]);
    return peak_mag_db - median;
}
