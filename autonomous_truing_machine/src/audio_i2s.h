/* Acoustic layer 1 on the target: INMP441 over ESP-IDF I2S (SPEC 9.3, 9.4, 9.4.1).
 *
 * The driver owns the DMA buffers (internal RAM). A drain task pinned to core 1 at high
 * priority reads them continuously into a small internal buffer and copies into a PSRAM
 * ring; that task has the hard deadline (SPEC 4.5). Capture requests are served from the
 * ring (the pre-trigger tail already in it, then live words) and complete on a semaphore,
 * so DSP never runs at drain priority. An `on_recv_q_ovf` event during a capture marks it
 * OVERRUN: the discontinuity corrupts the spectrum, so the capture is not analysed. */
#ifndef TRUING_AUDIO_I2S_H
#define TRUING_AUDIO_I2S_H

#include <stdbool.h>
#include <stdint.h>

#include "truing_hal/audio_source_if.h"

typedef struct {
    uint32_t dma_frame_num;        /* per descriptor; multiple of 3 for 24-bit data, <= 511 */
    uint32_t dma_desc_num;
    uint32_t pre_trigger_words;    /* ring depth handed to a capture before the trigger */
    uint32_t ring_words;           /* PSRAM ring capacity (>= pre_trigger_words + one read chunk) */
} truing_audio_i2s_config_t;

typedef struct {
    uint32_t words_drained;        /* since enable */
    uint32_t reads;
    uint32_t overrun_events;       /* driver on_recv_q_ovf callbacks, total */
    uint32_t read_timeouts;
    uint32_t max_read_gap_us;      /* longest interval between consecutive successful reads */
    uint32_t captures;
    uint32_t capture_overruns;
    bool     running;
} truing_audio_i2s_stats_t;

/* Creates the channel, starts the drain task and opens the source. `dsp_cancel` semantics per the
 * audio source contract. Returns false with `detail` when the channel cannot be created. */
bool truing_audio_i2s_init(truing_audio_source_if_t *self, const truing_audio_i2s_config_t *cfg, const char **detail);
void truing_audio_i2s_stats(const truing_audio_source_if_t *self, truing_audio_i2s_stats_t *out);
/* The last capture's own diagnostics (pre-roll delivered vs configured, this capture's overrun
 * delta, ring staleness and lifetime worst read gap) -- what self->capture_report exposes on the
 * vtable. Exposed here too so bring-up and other on-target callers needn't go through the vtable
 * indirection just to log it. */
void truing_audio_i2s_capture_report(const truing_audio_source_if_t *self, truing_audio_capture_report_t *out);
/* Stops the drain task and deletes the channel. */
void truing_audio_i2s_deinit(truing_audio_source_if_t *self);

#endif /* TRUING_AUDIO_I2S_H */
