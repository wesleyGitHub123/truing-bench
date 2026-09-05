#include "audio_i2s.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board/board_profile.h"
#include "truing/limits.h"

static const char *TAG = "audio_i2s";

#define DRAIN_TASK_PRIORITY   (configMAX_PRIORITIES - 2)   /* hard deadline: above everything but the system (SPEC 4.5) */
#define DRAIN_TASK_CORE       1
#define DRAIN_READ_TIMEOUT_MS 100u

typedef struct {
    i2s_chan_handle_t        rx;
    truing_audio_i2s_config_t cfg;
    int32_t                 *ring;            /* PSRAM */
    uint32_t                 ring_cap;
    volatile uint32_t        ring_head;       /* next write index */
    volatile uint32_t        ring_filled;     /* <= ring_cap */
    int32_t                 *chunk;           /* internal read buffer: dma_desc_num x dma_buffer_size */
    uint32_t                 chunk_words;
    TaskHandle_t             drain_task;
    SemaphoreHandle_t        lock;
    SemaphoreHandle_t        done;
    /* active capture (guarded by lock) */
    int32_t                 *cap_out;
    uint32_t                 cap_total;
    uint32_t                 cap_written;
    volatile bool            cap_active;
    volatile uint32_t        cap_overruns_at_start;
    volatile uint32_t        overrun_events;
    truing_audio_i2s_stats_t stats;
    int64_t                  last_read_us;
    bool                     open;
    volatile bool            stop;
} i2s_ctx_t;

static i2s_ctx_t s_ctx;   /* one front end per board */

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    i2s_ctx_t *c = (i2s_ctx_t *)user_ctx;
    c->overrun_events++;
    return false;
}

static void drain_task(void *arg)
{
    i2s_ctx_t *c = (i2s_ctx_t *)arg;
    const size_t chunk_bytes = (size_t)c->chunk_words * sizeof(int32_t);
    while (!c->stop) {
        size_t got = 0u;
        const esp_err_t e = i2s_channel_read(c->rx, c->chunk, chunk_bytes, &got, pdMS_TO_TICKS(DRAIN_READ_TIMEOUT_MS));
        if (e != ESP_OK || got == 0u) {
            c->stats.read_timeouts++;
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (c->last_read_us != 0) {
            const uint32_t gap = (uint32_t)(now - c->last_read_us);
            if (gap > c->stats.max_read_gap_us) c->stats.max_read_gap_us = gap;
        }
        c->last_read_us = now;
        const uint32_t n = (uint32_t)(got / sizeof(int32_t));
        c->stats.reads++;
        c->stats.words_drained += n;
        /* ring append (single producer) */
        for (uint32_t i = 0; i < n; ++i) {
            c->ring[c->ring_head] = c->chunk[i];
            c->ring_head = (c->ring_head + 1u) % c->ring_cap;
        }
        if (c->ring_filled < c->ring_cap) {
            c->ring_filled = (c->ring_filled + n) < c->ring_cap ? c->ring_filled + n : c->ring_cap;
        }
        /* live part of an active capture */
        if (c->cap_active) {
            xSemaphoreTake(c->lock, portMAX_DELAY);
            if (c->cap_active) {
                const uint32_t room = c->cap_total - c->cap_written;
                const uint32_t take = n < room ? n : room;
                memcpy(c->cap_out + c->cap_written, c->chunk, (size_t)take * sizeof(int32_t));
                c->cap_written += take;
                if (c->cap_written >= c->cap_total) {
                    c->cap_active = false;
                    xSemaphoreGive(c->done);
                }
            }
            xSemaphoreGive(c->lock);
        }
    }
    c->stats.running = false;
    vTaskDelete(NULL);
}

static void i2s_format(truing_audio_source_if_t *self, truing_audio_format_t *out)
{
    (void)self;
    truing_audio_system_format(out);
}

static truing_audio_result_t i2s_open(truing_audio_source_if_t *self)
{
    i2s_ctx_t *c = (i2s_ctx_t *)self->ctx;
    return (c != NULL && c->open) ? TRUING_AUDIO_OK : TRUING_AUDIO_ERR_NOT_OPEN;
}

static truing_audio_result_t i2s_capture(truing_audio_source_if_t *self, int32_t *words, uint32_t n_words,
                                         const volatile bool *cancel, uint32_t *n_captured)
{
    i2s_ctx_t *c = (i2s_ctx_t *)self->ctx;
    if (n_captured != NULL) *n_captured = 0u;
    if (c == NULL || !c->open || !c->stats.running) return TRUING_AUDIO_ERR_NOT_OPEN;
    if (n_words == 0u) return TRUING_AUDIO_ERR_CAPACITY;
    c->stats.captures++;
    /* Pre-trigger tail from the ring, then live words. The head is sampled ONCE: the drain task
     * writes the ring without the lock, so re-reading it inside the loop would let the tail slide
     * and skip or duplicate samples. With a ring far longer than the tail, the writer cannot
     * overtake this copy in the microseconds it takes. */
    xSemaphoreTake(c->lock, portMAX_DELAY);
    const uint32_t head = c->ring_head;
    uint32_t pre = c->cfg.pre_trigger_words < c->ring_filled ? c->cfg.pre_trigger_words : c->ring_filled;
    if (pre > n_words) pre = n_words;
    for (uint32_t i = 0; i < pre; ++i) {
        const uint32_t idx = (head + c->ring_cap - pre + i) % c->ring_cap;
        words[i] = c->ring[idx];
    }
    c->cap_out = words;
    c->cap_total = n_words;
    c->cap_written = pre;
    c->cap_overruns_at_start = c->overrun_events;
    xSemaphoreTake(c->done, 0);   /* clear a stale completion */
    c->cap_active = c->cap_written < c->cap_total;
    const bool already_done = !c->cap_active;
    xSemaphoreGive(c->lock);
    const uint32_t budget_ms = (uint32_t)((uint64_t)n_words * 1000u / TRUING_AUDIO_SAMPLE_RATE_HZ) + 1000u;
    const int64_t t0 = esp_timer_get_time();
    bool done = already_done;
    while (!done) {
        if (xSemaphoreTake(c->done, pdMS_TO_TICKS(10)) == pdTRUE) {
            done = true;
            break;
        }
        if (cancel != NULL && *cancel) {
            xSemaphoreTake(c->lock, portMAX_DELAY);
            c->cap_active = false;
            xSemaphoreGive(c->lock);
            return TRUING_AUDIO_ERR_CANCELLED;
        }
        if ((esp_timer_get_time() - t0) / 1000 > (int64_t)budget_ms) {
            xSemaphoreTake(c->lock, portMAX_DELAY);
            c->cap_active = false;
            xSemaphoreGive(c->lock);
            return TRUING_AUDIO_ERR_TIMEOUT;
        }
    }
    if (n_captured != NULL) *n_captured = n_words;
    if (c->overrun_events != c->cap_overruns_at_start) {
        c->stats.capture_overruns++;
        return TRUING_AUDIO_ERR_OVERRUN;
    }
    return TRUING_AUDIO_OK;
}

static void i2s_close(truing_audio_source_if_t *self)
{
    (void)self;   /* the channel keeps draining; deinit stops it */
}

bool truing_audio_i2s_init(truing_audio_source_if_t *self, const truing_audio_i2s_config_t *cfg, const char **detail)
{
    if (detail != NULL) *detail = "";
    if (self == NULL || cfg == NULL) return false;
    i2s_ctx_t *c = &s_ctx;
    if (c->open) {
        if (detail != NULL) *detail = "already open";
        return false;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    if (cfg->dma_frame_num == 0u || cfg->dma_frame_num > 511u || (cfg->dma_frame_num % 3u) != 0u ||
        cfg->dma_frame_num * (TRUING_AUDIO_SLOT_BITS / 8u) > 4092u || cfg->dma_desc_num < 2u) {
        if (detail != NULL) *detail = "DMA sizing violates SPEC 9.4.1";
        return false;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = cfg->dma_desc_num;
    cc.dma_frame_num = cfg->dma_frame_num;
    cc.auto_clear = false;
    esp_err_t e = i2s_new_channel(&cc, NULL, &c->rx);
    if (e != ESP_OK) {
        if (detail != NULL) *detail = esp_err_to_name(e);
        return false;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TRUING_AUDIO_SAMPLE_RATE_HZ),
        /* 32, not 24, and the difference is the whole sample. The INMP441 sends 24 valid bits
         * left-justified in a 32-bit slot, and the rest of this system is built on receiving
         * them that way: TRUING_AUDIO_SLOT_BITS is 32 and truing_audio_word_to_float() reads
         * `word >> 8`. Asking the driver for a 24-bit DATA width instead changes how it lays
         * samples into the DMA buffer, so `>> 8` then shifts a sample that was never
         * left-justified and every word is noise - full-scale, varying, and superficially
         * alive, which is the worst way for this to be wrong. Measured on the Nano: 24-bit
         * data width gave rms 0.520 (uniform noise is 0.577); 32-bit gives a real room. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_MIC_BCLK_GPIO,
            .ws = BOARD_I2S_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_MIC_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;   /* 24-bit data in 32-bit slots (SPEC 9.3) */
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;               /* INMP441 L/R tied low */
    e = i2s_channel_init_std_mode(c->rx, &sc);
    if (e != ESP_OK) {
        if (detail != NULL) *detail = esp_err_to_name(e);
        i2s_del_channel(c->rx);
        return false;
    }
    const i2s_event_callbacks_t cbs = { .on_recv = NULL, .on_recv_q_ovf = on_overflow, .on_sent = NULL, .on_send_q_ovf = NULL };
    i2s_channel_register_event_callback(c->rx, &cbs, c);
    /* read buffer >= dma_desc_num x dma_buffer_size (SPEC 9.4.1 sizing method) */
    c->chunk_words = cfg->dma_desc_num * cfg->dma_frame_num;
    c->chunk = heap_caps_malloc((size_t)c->chunk_words * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    c->ring_cap = cfg->ring_words > cfg->pre_trigger_words + c->chunk_words ? cfg->ring_words : cfg->pre_trigger_words + c->chunk_words;
    c->ring = heap_caps_malloc((size_t)c->ring_cap * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (c->chunk == NULL || c->ring == NULL) {
        if (detail != NULL) *detail = "buffer allocation";
        i2s_del_channel(c->rx);
        heap_caps_free(c->chunk);
        heap_caps_free(c->ring);
        return false;
    }
    c->lock = xSemaphoreCreateMutex();
    c->done = xSemaphoreCreateBinary();
    e = i2s_channel_enable(c->rx);
    if (e != ESP_OK) {
        if (detail != NULL) *detail = esp_err_to_name(e);
        i2s_del_channel(c->rx);
        return false;
    }
    c->stats.running = true;
    if (xTaskCreatePinnedToCore(drain_task, "i2s_drain", 4096, c, DRAIN_TASK_PRIORITY, &c->drain_task, DRAIN_TASK_CORE) != pdPASS) {
        if (detail != NULL) *detail = "drain task";
        c->stats.running = false;
        i2s_channel_disable(c->rx);
        i2s_del_channel(c->rx);
        return false;
    }
    c->open = true;
    self->impl_name = "audio_i2s_inmp441";
    self->source_impl = TRUING_SOURCE_REAL;
    self->format = i2s_format;
    self->open = i2s_open;
    self->capture = i2s_capture;
    self->close = i2s_close;
    self->ctx = c;
    ESP_LOGI(TAG, "I2S RX enabled: 48 kHz, 24-in-32 mono, mic on GPIO bclk=%d ws=%d din=%d "
                  "(every capture will be zero if this is not the wiring), dma %u x %u frames (%u B), drain on core %d prio %d, ring %u words in PSRAM",
             BOARD_I2S_MIC_BCLK_GPIO, BOARD_I2S_MIC_WS_GPIO, BOARD_I2S_MIC_DIN_GPIO,
             (unsigned)cfg->dma_desc_num, (unsigned)cfg->dma_frame_num, (unsigned)(cfg->dma_frame_num * 4u), DRAIN_TASK_CORE,
             DRAIN_TASK_PRIORITY, (unsigned)c->ring_cap);
    return true;
}

void truing_audio_i2s_stats(const truing_audio_source_if_t *self, truing_audio_i2s_stats_t *out)
{
    const i2s_ctx_t *c = self != NULL ? (const i2s_ctx_t *)self->ctx : NULL;
    if (out == NULL) return;
    if (c == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = c->stats;
    out->overrun_events = c->overrun_events;
}

void truing_audio_i2s_deinit(truing_audio_source_if_t *self)
{
    i2s_ctx_t *c = self != NULL ? (i2s_ctx_t *)self->ctx : NULL;
    if (c == NULL || !c->open) return;
    c->stop = true;
    while (c->stats.running) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    i2s_channel_disable(c->rx);
    i2s_del_channel(c->rx);
    heap_caps_free(c->chunk);
    heap_caps_free(c->ring);
    vSemaphoreDelete(c->lock);
    vSemaphoreDelete(c->done);
    c->open = false;
}
