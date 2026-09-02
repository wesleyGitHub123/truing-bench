#include "pluck_gpio.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool gpio_available(truing_pluck_if_t *self)
{
    const truing_pluck_gpio_ctx_t *c = (const truing_pluck_gpio_ctx_t *)self->ctx;
    return c != NULL && c->configured;
}

static bool gpio_fire(truing_pluck_if_t *self, float pulse_ms)
{
    truing_pluck_gpio_ctx_t *c = (truing_pluck_gpio_ctx_t *)self->ctx;
    if (c == NULL || !c->configured || !(pulse_ms > 0.0f)) return false;
    const int64_t t0 = esp_timer_get_time();
    gpio_set_level((gpio_num_t)c->gpio, 1);
    const uint32_t us = (uint32_t)(pulse_ms * 1000.0f);
    if (us >= 2000u) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000u));
    } else {
        esp_rom_delay_us(us);
    }
    gpio_set_level((gpio_num_t)c->gpio, 0);
    c->last_pulse_us_measured = (unsigned)(esp_timer_get_time() - t0);
    c->fires++;
    return true;
}

bool truing_pluck_gpio_init(truing_pluck_if_t *self, truing_pluck_gpio_ctx_t *ctx, int gpio)
{
    if (self == NULL || ctx == NULL) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->gpio = gpio;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ctx->configured = gpio_config(&io) == ESP_OK && gpio_set_level((gpio_num_t)gpio, 0) == ESP_OK;
    self->impl_name = "pluck_gpio";
    self->source_impl = TRUING_SOURCE_REAL;
    self->available = gpio_available;
    self->fire = gpio_fire;
    self->ctx = ctx;
    return ctx->configured;
}
