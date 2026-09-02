/* Non-hardware audio sources (SPEC 14.5): a recorded-buffer replay and a synthetic pluck. */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing_hal/audio_source_if.h"

void truing_audio_system_format(truing_audio_format_t *out)
{
    if (out != NULL) {
        out->sample_rate_hz = TRUING_AUDIO_SAMPLE_RATE_HZ;
        out->bit_depth = (uint8_t)TRUING_AUDIO_BIT_DEPTH;
        out->slot_bits = (uint8_t)TRUING_AUDIO_SLOT_BITS;
        out->channels = 1u;
    }
}

bool truing_audio_format_matches_system(const truing_audio_format_t *f)
{
    return f != NULL && f->sample_rate_hz == TRUING_AUDIO_SAMPLE_RATE_HZ && f->bit_depth == TRUING_AUDIO_BIT_DEPTH &&
           f->slot_bits == TRUING_AUDIO_SLOT_BITS && f->channels == 1u;
}

const char *truing_audio_result_str(truing_audio_result_t r)
{
    switch (r) {
    case TRUING_AUDIO_OK: return "OK";
    case TRUING_AUDIO_ERR_NOT_OPEN: return "NOT_OPEN";
    case TRUING_AUDIO_ERR_FORMAT: return "FORMAT";
    case TRUING_AUDIO_ERR_TIMEOUT: return "TIMEOUT";
    case TRUING_AUDIO_ERR_OVERRUN: return "OVERRUN";
    case TRUING_AUDIO_ERR_CANCELLED: return "CANCELLED";
    case TRUING_AUDIO_ERR_CAPACITY: return "CAPACITY";
    case TRUING_AUDIO_ERR_HARDWARE: return "HARDWARE";
    default: return "?";
    }
}

/* ---- recorded buffer ------------------------------------------------------------------- */
static void buf_format(truing_audio_source_if_t *self, truing_audio_format_t *out)
{
    const truing_audio_buffer_ctx_t *c = (const truing_audio_buffer_ctx_t *)self->ctx;
    if (out != NULL && c != NULL) {
        *out = c->format;
    }
}

static truing_audio_result_t buf_open(truing_audio_source_if_t *self)
{
    truing_audio_buffer_ctx_t *c = (truing_audio_buffer_ctx_t *)self->ctx;
    if (c == NULL || c->words == NULL) {
        return TRUING_AUDIO_ERR_HARDWARE;
    }
    if (!truing_audio_format_matches_system(&c->format)) {
        return TRUING_AUDIO_ERR_FORMAT;   /* SPEC 14.4: rejected, not resampled */
    }
    c->open = true;
    return TRUING_AUDIO_OK;
}

static truing_audio_result_t buf_capture(truing_audio_source_if_t *self, int32_t *words, uint32_t n_words,
                                         const volatile bool *cancel, uint32_t *n_captured)
{
    truing_audio_buffer_ctx_t *c = (truing_audio_buffer_ctx_t *)self->ctx;
    if (n_captured != NULL) *n_captured = 0u;
    if (c == NULL || !c->open) {
        return TRUING_AUDIO_ERR_NOT_OPEN;
    }
    c->captures++;
    if (cancel != NULL && *cancel) {
        return TRUING_AUDIO_ERR_CANCELLED;
    }
    if (c->inject_overrun) {
        c->inject_overrun = false;
        return TRUING_AUDIO_ERR_OVERRUN;
    }
    /* Replay from the start; a request longer than the recording is padded with silence and
     * reported as fewer captured words, so the caller knows the tail is not recorded audio. */
    const uint32_t n = n_words < c->n_words ? n_words : c->n_words;
    memcpy(words, c->words, (size_t)n * sizeof(int32_t));
    if (n < n_words) {
        memset(words + n, 0, (size_t)(n_words - n) * sizeof(int32_t));
    }
    if (n_captured != NULL) *n_captured = n;
    return TRUING_AUDIO_OK;
}

static void buf_close(truing_audio_source_if_t *self)
{
    truing_audio_buffer_ctx_t *c = (truing_audio_buffer_ctx_t *)self->ctx;
    if (c != NULL) c->open = false;
}

void truing_audio_buffer_init(truing_audio_source_if_t *self, truing_audio_buffer_ctx_t *ctx, const int32_t *words,
                              uint32_t n_words, const truing_audio_format_t *format)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->words = words;
    ctx->n_words = n_words;
    if (format != NULL) {
        ctx->format = *format;
    } else {
        truing_audio_system_format(&ctx->format);
    }
    self->impl_name = "audio_recorded_buffer";
    self->source_impl = TRUING_SOURCE_RECORDED;
    self->format = buf_format;
    self->open = buf_open;
    self->capture = buf_capture;
    self->close = buf_close;
    self->ctx = ctx;
}

/* ---- synthetic pluck ------------------------------------------------------------------- */
static void syn_format(truing_audio_source_if_t *self, truing_audio_format_t *out)
{
    const truing_audio_synthetic_ctx_t *c = (const truing_audio_synthetic_ctx_t *)self->ctx;
    if (out != NULL && c != NULL) *out = c->format;
}

static truing_audio_result_t syn_open(truing_audio_source_if_t *self)
{
    truing_audio_synthetic_ctx_t *c = (truing_audio_synthetic_ctx_t *)self->ctx;
    if (c == NULL) return TRUING_AUDIO_ERR_HARDWARE;
    if (!truing_audio_format_matches_system(&c->format)) return TRUING_AUDIO_ERR_FORMAT;
    c->open = true;
    return TRUING_AUDIO_OK;
}

static float lcg_uniform(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return ((float)(*seed >> 8) / 16777216.0f) - 0.5f;   /* [-0.5, 0.5) */
}

static truing_audio_result_t syn_capture(truing_audio_source_if_t *self, int32_t *words, uint32_t n_words,
                                         const volatile bool *cancel, uint32_t *n_captured)
{
    truing_audio_synthetic_ctx_t *c = (truing_audio_synthetic_ctx_t *)self->ctx;
    if (n_captured != NULL) *n_captured = 0u;
    if (c == NULL || !c->open) return TRUING_AUDIO_ERR_NOT_OPEN;
    c->captures++;
    if (cancel != NULL && *cancel) return TRUING_AUDIO_ERR_CANCELLED;
    const float fs = (float)c->format.sample_rate_hz;
    const uint32_t onset = (uint32_t)(c->onset_delay_s * fs);
    uint32_t seed = c->seed;
    for (uint32_t i = 0; i < n_words; ++i) {
        float v = 0.0f;
        if (i >= onset) {
            const float t = (float)(i - onset) / fs;
            const float decay = c->tau_s > 0.0f ? expf(-t / c->tau_s) : 1.0f;
            v = c->amplitude * decay * sinf(2.0f * 3.14159265f * c->f1_hz * t);
            if (c->f2_hz > 0.0f) {
                v += c->amplitude * c->amp2_rel * decay * sinf(2.0f * 3.14159265f * c->f2_hz * t);
            }
        }
        if (c->noise_rms > 0.0f) {
            /* sum of 4 uniforms ~ near-Gaussian with rms 1/sqrt(3) -> scale to the requested rms */
            const float g = (lcg_uniform(&seed) + lcg_uniform(&seed) + lcg_uniform(&seed) + lcg_uniform(&seed)) * 1.7320508f;
            v += c->noise_rms * g;
        }
        if (v > 0.999999f) v = 0.999999f;
        if (v < -1.0f) v = -1.0f;
        const int32_t s24 = (int32_t)lrintf(v * 8388608.0f);
        words[i] = (int32_t)((uint32_t)s24 << 8);
    }
    if (n_captured != NULL) *n_captured = n_words;
    return TRUING_AUDIO_OK;
}

static void syn_close(truing_audio_source_if_t *self)
{
    truing_audio_synthetic_ctx_t *c = (truing_audio_synthetic_ctx_t *)self->ctx;
    if (c != NULL) c->open = false;
}

void truing_audio_synthetic_init(truing_audio_source_if_t *self, truing_audio_synthetic_ctx_t *ctx, float f1_hz,
                                 float amplitude, float tau_s, float onset_delay_s, float noise_rms)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    truing_audio_system_format(&ctx->format);
    ctx->f1_hz = f1_hz;
    ctx->amplitude = amplitude;
    ctx->tau_s = tau_s;
    ctx->onset_delay_s = onset_delay_s;
    ctx->noise_rms = noise_rms;
    ctx->seed = 12345u;
    self->impl_name = "audio_synthetic_pluck";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->format = syn_format;
    self->open = syn_open;
    self->capture = syn_capture;
    self->close = syn_close;
    self->ctx = ctx;
}
