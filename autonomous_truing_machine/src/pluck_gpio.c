/* vTaskDelay(N ticks) guarantees AT LEAST N ticks -- if a higher-priority task (httpd at 5,
 * wire_tx at 3) is running when the delay expires, the gpio_set_level(0) that ends the pulse
 * waits behind it. Under WiFi/UI load the actual pulse can stretch past what was commanded, by
 * however long those tasks were busy, with no bound the caller can reason about. That stretch
 * is exactly the uncontrolled variable a pulse-width sweep needs not to have.
 *
 * The fix moves the falling edge off the task scheduler entirely: a gptimer one-shot alarm
 * clears the pin from its own ISR, which preempts ordinary tasks regardless of their priority.
 * fire() still blocks the caller until the pulse is confirmed done (a binary semaphore, given
 * from the ISR) -- the contract at the pluck_if.h seam is unchanged, only how the timing inside
 * it is achieved. The measured width comes from timestamps taken at GPIO-high and inside the
 * ISR at GPIO-low, so what's reported is the hardware-timed pulse, not however long the caller's
 * task took to wake back up afterward. */
#include "pluck_gpio.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "truing/config.h"   /* TRUING_EXCITATION_PULSE_MAX_MS -- the same sanity bound the
                                 excitation profile validates against, checked again here rather
                                 than trusted */

static bool gpio_available(truing_pluck_if_t *self)
{
    const truing_pluck_gpio_ctx_t *c = (const truing_pluck_gpio_ctx_t *)self->ctx;
    return c != NULL && c->configured;
}

/* Runs in ISR context. gpio_set_level() is a direct GPIO-register write (no lock, no blocking
 * call) and is documented safe to call from an ISR. */
static bool IRAM_ATTR alarm_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    (void)timer;
    (void)edata;
    truing_pluck_gpio_ctx_t *c = (truing_pluck_gpio_ctx_t *)user_ctx;
    gpio_set_level((gpio_num_t)c->gpio, 0);
    c->last_pulse_us_measured = (unsigned)(esp_timer_get_time() - c->t_on_us);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(c->done_sem, &woken);
    return woken == pdTRUE;
}

static bool gpio_fire(truing_pluck_if_t *self, float pulse_ms)
{
    truing_pluck_gpio_ctx_t *c = (truing_pluck_gpio_ctx_t *)self->ctx;
    if (c == NULL || !c->configured || !(pulse_ms > 0.0f)) {
        return false;
    }
    /* The excitation profile already validates pulse_ms against this same bound; checked again
     * here rather than trusted, since this function has no way to know whether its caller did. */
    if (!(pulse_ms <= TRUING_EXCITATION_PULSE_MAX_MS)) {
        return false;
    }
    const uint64_t us = (uint64_t)(pulse_ms * 1000.0f);
    if (us == 0u) {
        return false;
    }

    const gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    if (gptimer_set_alarm_action(c->timer, &alarm_cfg) != ESP_OK || gptimer_set_raw_count(c->timer, 0) != ESP_OK) {
        return false;
    }
    /* Drain a stale signal so a previous fire's completion (e.g. after a timeout below) can
     * never be mistaken for this one's. */
    xSemaphoreTake(c->done_sem, 0);

    c->t_on_us = esp_timer_get_time();
    gpio_set_level((gpio_num_t)c->gpio, 1);
    if (gptimer_start(c->timer) != ESP_OK) {
        gpio_set_level((gpio_num_t)c->gpio, 0);
        return false;
    }

    /* fire() still blocks -- one call in, the excitation is over when it returns -- but what it
     * waits ON is the ISR's signal, not a task-scheduled wakeup. The timeout is generous (2x
     * commanded plus a fixed margin) and exists only to catch a timer fault instead of hanging
     * forever; on timeout the pin is forced off by hand and the fire is reported as failed,
     * since the hardware-timed guarantee did not hold. */
    const uint32_t timeout_ms = (uint32_t)(pulse_ms * 2.0f) + 100u;
    const bool completed = xSemaphoreTake(c->done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    gptimer_stop(c->timer);
    if (!completed) {
        gpio_set_level((gpio_num_t)c->gpio, 0);
        return false;
    }
    c->fires++;
    return true;
}

static bool gpio_fire_report(truing_pluck_if_t *self, truing_pluck_fire_report_t *out)
{
    const truing_pluck_gpio_ctx_t *c = (const truing_pluck_gpio_ctx_t *)self->ctx;
    if (c == NULL || out == NULL || c->fires == 0u) {
        return false;
    }
    out->pulse_us_measured = c->last_pulse_us_measured;
    out->hardware_timed = true;
    return true;
}

bool truing_pluck_gpio_init(truing_pluck_if_t *self, truing_pluck_gpio_ctx_t *ctx, int gpio)
{
    if (self == NULL || ctx == NULL) {
        return false;
    }
    /* Zeroed first so every field this function does not explicitly set -- including any added
     * later -- starts at a safe default rather than whatever was on the caller's stack. */
    memset(self, 0, sizeof(*self));
    memset(ctx, 0, sizeof(*ctx));
    ctx->gpio = gpio;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    bool ok = gpio_config(&io) == ESP_OK && gpio_set_level((gpio_num_t)gpio, 0) == ESP_OK;

    ctx->done_sem = ok ? xSemaphoreCreateBinary() : NULL;
    ok = ok && ctx->done_sem != NULL;

    if (ok) {
        const gptimer_config_t timer_cfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000u,   /* 1 tick = 1 us */
        };
        ok = gptimer_new_timer(&timer_cfg, &ctx->timer) == ESP_OK;
    }
    if (ok) {
        const gptimer_event_callbacks_t cbs = { .on_alarm = alarm_isr };
        ok = gptimer_register_event_callbacks(ctx->timer, &cbs, ctx) == ESP_OK;
    }
    if (ok) {
        ok = gptimer_enable(ctx->timer) == ESP_OK;
    }

    ctx->configured = ok;
    self->impl_name = "pluck_gpio";
    self->source_impl = TRUING_SOURCE_REAL;
    self->available = gpio_available;
    self->fire = gpio_fire;
    self->fire_report = gpio_fire_report;
    self->ctx = ctx;
    return ctx->configured;
}
