/* Solenoid bench for both driver channels, driven by hand from the serial console.
 * LEFT = GPIO 9 (D6), RIGHT = GPIO 10 (D7) on the Nano ESP32 -- the pins the two-station
 * firmware declares as BOARD_PLUCK_ACTUATOR_LEFT/RIGHT_GPIO. Same GPIO config as pluck_gpio.c
 * (output, internal pulldown), none of the acoustic or orchestrator code.
 *
 * Nothing fires on boot. Every activation is one keystroke, so a single observation can be
 * repeated as often as it takes to be sure of it:
 *
 *   l / r    one shot on LEFT / RIGHT at the current shot width
 *   L / R    500 ms hold on LEFT / RIGHT (read V_DS on the meter during it)
 *   a        10 alternating shots, LEFT first, 3 s apart; any key aborts
 *   + / -    shot width +/- 5 ms (5..100 ms)
 *   s        status: shot width and per-channel fire counts
 *   ? / h    this help
 *
 * Every pulse logs its channel, that channel's running count and its esp_timer-measured width,
 * not the commanded one. Keys typed while a pulse is out are discarded, so holding a key down
 * cannot queue a burst, and fires are spaced at least MIN_GAP_MS apart. */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LEFT_GPIO      9    /* D6 */
#define RIGHT_GPIO     10   /* D7 */
#define HOLD_PULSE_MS  500u
#define SHOT_MS_INIT   20u
#define SHOT_MS_STEP   5u
#define SHOT_MS_MIN    5u
#define SHOT_MS_MAX    100u
#define RUN_SHOTS      10
#define RUN_GAP_MS     3000u
#define MIN_GAP_MS     250u

static const char *TAG = "solenoid_bench";

typedef struct {
    const char *name;
    int gpio;
    unsigned fires;
} channel_t;

static channel_t g_left = { "LEFT", LEFT_GPIO, 0u };
static channel_t g_right = { "RIGHT", RIGHT_GPIO, 0u };
static uint32_t g_shot_ms = SHOT_MS_INIT;
static int64_t g_last_end_us;

static void flush_input(void)
{
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) > 0) {
    }
}

static void fire(channel_t *ch, uint32_t pulse_ms)
{
    const int64_t since_ms = (esp_timer_get_time() - g_last_end_us) / 1000;
    if (since_ms < (int64_t)MIN_GAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(MIN_GAP_MS - (uint32_t)since_ms));
    }
    const int64_t t0 = esp_timer_get_time();
    gpio_set_level(ch->gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));
    gpio_set_level(ch->gpio, 0);
    g_last_end_us = esp_timer_get_time();
    ch->fires++;
    ESP_LOGI(TAG, "%s #%u (GPIO %d): commanded %u ms, measured %lld us", ch->name, ch->fires, ch->gpio,
             (unsigned)pulse_ms, (long long)(g_last_end_us - t0));
    flush_input();
}

static void help(void)
{
    ESP_LOGI(TAG, "keys: l/r shot LEFT/RIGHT | L/R 500 ms hold | a 10 alternating (any key aborts) | "
                  "+/- shot width | s status | ? help");
}

static void status(void)
{
    ESP_LOGI(TAG, "shot width %u ms | LEFT fired %u | RIGHT fired %u", (unsigned)g_shot_ms, g_left.fires,
             g_right.fires);
}

static void alternating_run(void)
{
    ESP_LOGI(TAG, "alternating run: %d shots at %u ms, %u ms apart, LEFT first -- any key aborts", RUN_SHOTS,
             (unsigned)g_shot_ms, (unsigned)RUN_GAP_MS);
    for (int i = 1; i <= RUN_SHOTS; i++) {
        const bool left = (i % 2) == 1;
        ESP_LOGI(TAG, "run shot %d/%d -> %s", i, RUN_SHOTS, left ? "LEFT" : "RIGHT");
        fire(left ? &g_left : &g_right, g_shot_ms);
        uint8_t c;
        if (i < RUN_SHOTS && usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(RUN_GAP_MS)) > 0) {
            ESP_LOGW(TAG, "run aborted after shot %d/%d", i, RUN_SHOTS);
            flush_input();
            return;
        }
    }
    ESP_LOGI(TAG, "alternating run done");
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

    usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj));
    usb_serial_jtag_vfs_use_driver();

    ESP_LOGI(TAG, "solenoid bench, manual -- LEFT GPIO %d (D6), RIGHT GPIO %d (D7). Nothing fires until a key.",
             LEFT_GPIO, RIGHT_GPIO);
    help();
    status();

    while (1) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) <= 0) {
            continue;
        }
        switch (c) {
        case 'l': fire(&g_left, g_shot_ms); break;
        case 'r': fire(&g_right, g_shot_ms); break;
        case 'L': ESP_LOGI(TAG, "LEFT hold -- read V_DS now"); fire(&g_left, HOLD_PULSE_MS); break;
        case 'R': ESP_LOGI(TAG, "RIGHT hold -- read V_DS now"); fire(&g_right, HOLD_PULSE_MS); break;
        case 'a': alternating_run(); break;
        case '+':
            g_shot_ms = (g_shot_ms + SHOT_MS_STEP > SHOT_MS_MAX) ? SHOT_MS_MAX : g_shot_ms + SHOT_MS_STEP;
            status();
            break;
        case '-':
            g_shot_ms = (g_shot_ms < SHOT_MS_MIN + SHOT_MS_STEP) ? SHOT_MS_MIN : g_shot_ms - SHOT_MS_STEP;
            status();
            break;
        case 's': status(); break;
        case '?': case 'h': help(); break;
        case '\r': case '\n': break;
        default: ESP_LOGW(TAG, "unknown key 0x%02x -- ? for help", c); break;
        }
    }
}
