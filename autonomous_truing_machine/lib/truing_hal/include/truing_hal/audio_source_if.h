/**
 * @file audio_source_if.h
 * Acoustic layer 1, the front end (SPEC 9.2 seam 1, 9.3): transducer -> RawAudioBuffer.
 *
 * One fixed system format: 48 kHz, 24-bit samples in 32-bit I2S slots, delivered as
 * int32 words with the sample in the upper 24 bits (what the ESP-IDF I2S driver
 * produces). A source that cannot deliver that format is REFUSED at open, never
 * resampled (SPEC 14.4). The I2S implementation lives with the firmware; the recorded
 * and synthetic implementations here let layers 2-4 run with no hardware (SPEC 14.5).
 */
#ifndef TRUING_HAL_AUDIO_SOURCE_IF_H
#define TRUING_HAL_AUDIO_SOURCE_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/limits.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t  bit_depth;
    uint8_t  slot_bits;
    uint8_t  channels;
} truing_audio_format_t;

typedef enum {
    TRUING_AUDIO_OK = 0,
    TRUING_AUDIO_ERR_NOT_OPEN,
    TRUING_AUDIO_ERR_FORMAT,       /* source cannot deliver the fixed system format */
    TRUING_AUDIO_ERR_TIMEOUT,      /* no data within the expected time */
    TRUING_AUDIO_ERR_OVERRUN,      /* samples were dropped: the capture is corrupt (SPEC 9.4) */
    TRUING_AUDIO_ERR_CANCELLED,
    TRUING_AUDIO_ERR_CAPACITY,
    TRUING_AUDIO_ERR_HARDWARE,
} truing_audio_result_t;

typedef struct truing_audio_source_if truing_audio_source_if_t;

/* What the front end can additionally say about the capture just completed, when it can
 * measure it. Additive and optional, the same shape and NULL-safety as
 * truing_pluck_if_t.fire_report (pluck_if.h): a source that cannot report leaves the vtable
 * slot NULL, and a caller must check for that before calling through it. */
typedef struct {
    uint32_t pre_roll_words_delivered;   /* pre-trigger words actually copied into this capture */
    uint32_t pre_roll_words_configured;  /* the source's configured pre-trigger depth, for shortfall */
    uint32_t capture_overrun_events;     /* per-capture delta of driver overrun events (not just a bool) */
    uint32_t ring_age_us;                /* how stale the ring was when this capture's pre-roll tail was read */
    uint32_t worst_read_gap_us;          /* driver-lifetime worst gap between drain reads, as of this capture */
} truing_audio_capture_report_t;

struct truing_audio_source_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    /* The format this source delivers. */
    void (*format)(truing_audio_source_if_t *self, truing_audio_format_t *out);
    /* Prepare the capture path; false with a reason when the source cannot deliver the format. */
    truing_audio_result_t (*open)(truing_audio_source_if_t *self);
    /* Blocking capture of exactly n_words starting now. Returns OVERRUN if the front end dropped
     * samples during the capture (the words are then not to be analysed). `cancel` is polled. */
    truing_audio_result_t (*capture)(truing_audio_source_if_t *self, int32_t *words, uint32_t n_words,
                                     const volatile bool *cancel, uint32_t *n_captured);
    /* Optional; NULL if this implementation cannot report per-capture diagnostics. Describes
     * the capture() just completed, not the one about to happen -- call it only after
     * capture() has returned. */
    bool (*capture_report)(truing_audio_source_if_t *self, truing_audio_capture_report_t *out);
    void (*close)(truing_audio_source_if_t *self);
    void *ctx;
};

/* The fixed system format (SPEC 9.3). */
void truing_audio_system_format(truing_audio_format_t *out);
bool truing_audio_format_matches_system(const truing_audio_format_t *f);
/* I2S word (24-bit sample in the upper bits) -> float full scale 1.0, as the research reference scales. */
static inline float truing_audio_word_to_float(int32_t word)
{
    return (float)(word >> 8) * (1.0f / 8388608.0f);
}
const char *truing_audio_result_str(truing_audio_result_t r);

/* ---- Recorded buffer: replays int32 words (RECORDED source) -------------------------- */
typedef struct {
    const int32_t        *words;
    uint32_t              n_words;
    truing_audio_format_t format;      /* what the recording claims; a mismatch is refused at open */
    bool                  open;
    bool                  inject_overrun;   /* test knob: report OVERRUN on the next capture */
    uint32_t              captures;
} truing_audio_buffer_ctx_t;

void truing_audio_buffer_init(truing_audio_source_if_t *self, truing_audio_buffer_ctx_t *ctx, const int32_t *words,
                              uint32_t n_words, const truing_audio_format_t *format);

/* ---- Synthetic pluck: decaying sinusoid(s) with an onset delay and noise (SYNTHETIC) ---- */
typedef struct {
    truing_audio_format_t format;
    float    f1_hz;
    float    f2_hz;                 /* 0 = no second partial */
    float    amp2_rel;              /* second partial amplitude relative to the first */
    float    amplitude;             /* full scale 1.0 */
    float    tau_s;                 /* exponential decay time constant */
    float    onset_delay_s;         /* silence before the pluck */
    float    noise_rms;             /* white noise, full scale 1.0 */
    uint32_t seed;
    bool     open;
    uint32_t captures;
} truing_audio_synthetic_ctx_t;

void truing_audio_synthetic_init(truing_audio_source_if_t *self, truing_audio_synthetic_ctx_t *ctx, float f1_hz,
                                 float amplitude, float tau_s, float onset_delay_s, float noise_rms);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_AUDIO_SOURCE_IF_H */
