/* Solenoid bench smoke test for both driver channels, independent of the switching part.
 * LEFT = GPIO 9 (D6), RIGHT = GPIO 10 (D7) on the Nano ESP32 -- the pins the two-station
 * firmware will declare as BOARD_PLUCK_ACTUATOR_LEFT/RIGHT_GPIO. Same GPIO config as
 * pluck_gpio.c (output, internal pulldown), none of the acoustic or orchestrator code.
 *
 * Sequence on boot:
 *   1. 5 s pause -- get clear, meter ready.
 *   2. LEFT 500 ms hold pulse -- read V_DS on the LEFT MOSFET.
 *   3. 5 s pause.
 *   4. RIGHT 500 ms hold pulse -- read V_DS on the RIGHT MOSFET.
 *   5. 5 s pause.
 *   6. 10 shots at 20 ms, alternating LEFT, RIGHT, ..., 3 s apart -- each click must come
 *      from the solenoid the log names.
 *   7. Idle.
 * Every pulse logs its measured duration (esp_timer), not the commanded one. */
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LEFT_GPIO      9    /* D6 */
#define RIGHT_GPIO     10   /* D7 */
#define HOLD_PULSE_MS  500u
#define REAL_PULSE_MS  20u
#define SHOT_COUNT     10
#define SHOT_GAP_MS    3000u

static const char *TAG = "solenoid_bench";

static void fire_once(const char *name, int gpio, uint32_t pulse_ms)
{
    const int64_t t0 = esp_timer_get_time();
    gpio_set_level(gpio, 1);
    if (pulse_ms >= 2u) {
        vTaskDelay(pdMS_TO_TICKS(pulse_ms));
    } else {
        esp_rom_delay_us(pulse_ms * 1000u);
    }
    gpio_set_level(gpio, 0);
    const int64_t dt_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "  %s (GPIO %d) fired: commanded %u ms, measured %lld us", name, gpio, (unsigned)pulse_ms,
             (long long)dt_us);
}

void app_main(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << LEFT_GPIO) | (1ULL << RIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(LEFT_GPIO, 0);
    gpio_set_level(RIGHT_GPIO, 0);

    ESP_LOGI(TAG, "solenoid bench smoke test -- LEFT GPIO %d (D6), RIGHT GPIO %d (D7)", LEFT_GPIO, RIGHT_GPIO);
    ESP_LOGI(TAG, "5 s: get clear, meter on the LEFT MOSFET drain/source...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "LEFT hold pulse (%u ms) -- read V_DS now", HOLD_PULSE_MS);
    fire_once("LEFT", LEFT_GPIO, HOLD_PULSE_MS);

    ESP_LOGI(TAG, "5 s: move the meter to the RIGHT MOSFET drain/source...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "RIGHT hold pulse (%u ms) -- read V_DS now", HOLD_PULSE_MS);
    fire_once("RIGHT", RIGHT_GPIO, HOLD_PULSE_MS);

    ESP_LOGI(TAG, "5 s pause before the alternating run...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    for (int i = 1; i <= SHOT_COUNT; i++) {
        const bool left = (i % 2) == 1;
        ESP_LOGI(TAG, "shot %d/%d", i, SHOT_COUNT);
        fire_once(left ? "LEFT" : "RIGHT", left ? LEFT_GPIO : RIGHT_GPIO, REAL_PULSE_MS);
        vTaskDelay(pdMS_TO_TICKS(SHOT_GAP_MS));
    }

    ESP_LOGI(TAG, "done -- %d alternating shots at %u ms. Idle now, safe to power off.", SHOT_COUNT, REAL_PULSE_MS);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
