/* Solenoid bench for both driver channels, driven by hand from the serial console.
 * LEFT = GPIO 9 (D6), RIGHT = GPIO 10 (D7) on the Nano ESP32 -- the pins the two-station
 * firmware declares as BOARD_PLUCK_ACTUATOR_LEFT/RIGHT_GPIO. Same gate wiring as pluck_gpio.c
 * (100-150 ohm series gate resistor, 10 k gate-source pulldown), none of the acoustic or
 * orchestrator code.
 *
 * The gate is driven through LEDC (PWM) instead of a plain GPIO write. Turn-on is always to
 * full duty -- there is no reason to soften the push, and a slow turn-on would just cost force
 * and standoff. What's adjustable is turn-off: a push solenoid retracts on its own spring once
 * current stops, and how fast that current collapses (hard cutoff vs. a duty ramp through the
 * flyback path) is a real, currently uncontrolled variable in how sharp or soft the release
 * looks and sounds. "release ms" is that ramp length; 0 means the original instant cutoff.
 *
 * Nothing fires on boot. Every activation is one keystroke, so a single observation can be
 * repeated as often as it takes to be sure of it:
 *
 *   l / r    one shot on LEFT / RIGHT at the current shot width and release ramp
 *   L / R    500 ms hold on LEFT / RIGHT (read V_DS on the meter during it)
 *   a        10 alternating shots, LEFT first, 3 s apart; any key aborts
 *   + / -    shot width +/- 5 ms (5..100 ms)
 *   [ / ]    release ramp -/+ 2 ms (0..50 ms; 0 = instant cutoff, the old behaviour)
 *   s        status: shot width, release ramp, per-channel fire counts
 *   ? / h    this help
 *
 * Every pulse logs its channel, that channel's running count, the esp_timer-measured hold
 * width and the release ramp actually used. Keys typed while a pulse is out are discarded, so
 * holding a key down cannot queue a burst, and fires are spaced at least MIN_GAP_MS apart. */
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LEFT_GPIO       9    /* D6 */
#define RIGHT_GPIO      10   /* D7 */
#define HOLD_PULSE_MS   500u
#define SHOT_MS_INIT    20u
#define SHOT_MS_STEP    5u
#define SHOT_MS_MIN     5u
#define SHOT_MS_MAX     100u
#define RELEASE_MS_INIT 0u
#define RELEASE_MS_STEP 2u
#define RELEASE_MS_MAX  50u
#define RUN_SHOTS       10
#define RUN_GAP_MS      3000u
#define MIN_GAP_MS      250u

#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_RES_BITS   LEDC_TIMER_10_BIT
#define LEDC_FULL_DUTY  ((1u << 10) - 1u)  /* matches LEDC_RES_BITS */
#define LEDC_FREQ_HZ    20000u             /* above audible range; far above coil bandwidth */
#define RAMP_STEPS      20u

static const char *TAG = "solenoid_bench";

typedef struct {
    const char *name;
    int gpio;
    ledc_channel_t channel;
    unsigned fires;
} channel_t;

static channel_t g_left = { "LEFT", LEFT_GPIO, LEDC_CHANNEL_0, 0u };
static channel_t g_right = { "RIGHT", RIGHT_GPIO, LEDC_CHANNEL_1, 0u };
static uint32_t g_shot_ms = SHOT_MS_INIT;
static uint32_t g_release_ms = RELEASE_MS_INIT;
static int64_t g_last_end_us;

static void flush_input(void)
{
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) > 0) {
    }
}

/* Duty ramp from full to 0 over release_ms, in RAMP_STEPS steps. release_ms == 0 is a single
 * hard cutoff -- the original behaviour, kept as the default. */
static void release(channel_t *ch, uint32_t release_ms)
{
    if (release_ms == 0u) {
        ledc_set_duty_and_update(LEDC_MODE, ch->channel, 0, 0);
        return;
    }
    const uint32_t step_us = (release_ms * 1000u) / RAMP_STEPS;
    for (uint32_t i = 1; i <= RAMP_STEPS; i++) {
        const uint32_t duty = (i >= RAMP_STEPS) ? 0u : LEDC_FULL_DUTY - (LEDC_FULL_DUTY * i) / RAMP_STEPS;
        ledc_set_duty_and_update(LEDC_MODE, ch->channel, duty, 0);
        esp_rom_delay_us(step_us);
    }
}

static void fire(channel_t *ch, uint32_t pulse_ms, uint32_t release_ms)
{
    const int64_t since_ms = (esp_timer_get_time() - g_last_end_us) / 1000;
    if (since_ms < (int64_t)MIN_GAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(MIN_GAP_MS - (uint32_t)since_ms));
    }
    const int64_t t0 = esp_timer_get_time();
    ledc_set_duty_and_update(LEDC_MODE, ch->channel, LEDC_FULL_DUTY, 0);
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));
    const int64_t t_hold_end = esp_timer_get_time();
    release(ch, release_ms);
    g_last_end_us = esp_timer_get_time();
    ch->fires++;
    ESP_LOGI(TAG, "%s #%u (GPIO %d): commanded %u ms hold + %u ms release, measured hold %lld us, "
                  "total %lld us", ch->name, ch->fires, ch->gpio, (unsigned)pulse_ms, (unsigned)release_ms,
             (long long)(t_hold_end - t0), (long long)(g_last_end_us - t0));
    flush_input();
}

static void help(void)
{
    ESP_LOGI(TAG, "keys: l/r shot LEFT/RIGHT | L/R 500 ms hold | a 10 alternating (any key aborts) | "
                  "+/- shot width | [/] release ramp | s status | ? help");
}

static void status(void)
{
    ESP_LOGI(TAG, "shot width %u ms | release ramp %u ms (0 = instant cutoff) | LEFT fired %u | RIGHT fired %u",
             (unsigned)g_shot_ms, (unsigned)g_release_ms, g_left.fires, g_right.fires);
}

static void alternating_run(void)
{
    ESP_LOGI(TAG, "alternating run: %d shots at %u ms hold + %u ms release, %u ms apart, LEFT first -- "
                  "any key aborts", RUN_SHOTS, (unsigned)g_shot_ms, (unsigned)g_release_ms,
             (unsigned)RUN_GAP_MS);
    for (int i = 1; i <= RUN_SHOTS; i++) {
        const bool left = (i % 2) == 1;
        ESP_LOGI(TAG, "run shot %d/%d -> %s", i, RUN_SHOTS, left ? "LEFT" : "RIGHT");
        fire(left ? &g_left : &g_right, g_shot_ms, g_release_ms);
        uint8_t c;
        if (i < RUN_SHOTS && usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(RUN_GAP_MS)) > 0) {
            ESP_LOGW(TAG, "run aborted after shot %d/%d", i, RUN_SHOTS);
            flush_input();
            return;
        }
    }
    ESP_LOGI(TAG, "alternating run done");
}

static void ledc_setup(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RES_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t left_ch = {
        .gpio_num = LEFT_GPIO,
        .speed_mode = LEDC_MODE,
        .channel = g_left.channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&left_ch));

    const ledc_channel_config_t right_ch = {
        .gpio_num = RIGHT_GPIO,
        .speed_mode = LEDC_MODE,
        .channel = g_right.channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&right_ch));
}

void app_main(void)
{
    /* Hold both lines low with the internal pulldown for the brief window before LEDC takes
     * the pins over -- idle-low at boot no matter what, same intent as pluck_gpio.c. */
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

    ledc_setup();

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
        case 'l': fire(&g_left, g_shot_ms, g_release_ms); break;
        case 'r': fire(&g_right, g_shot_ms, g_release_ms); break;
        case 'L': ESP_LOGI(TAG, "LEFT hold -- read V_DS now"); fire(&g_left, HOLD_PULSE_MS, g_release_ms); break;
        case 'R': ESP_LOGI(TAG, "RIGHT hold -- read V_DS now"); fire(&g_right, HOLD_PULSE_MS, g_release_ms); break;
        case 'a': alternating_run(); break;
        case '+':
            g_shot_ms = (g_shot_ms + SHOT_MS_STEP > SHOT_MS_MAX) ? SHOT_MS_MAX : g_shot_ms + SHOT_MS_STEP;
            status();
            break;
        case '-':
            g_shot_ms = (g_shot_ms < SHOT_MS_MIN + SHOT_MS_STEP) ? SHOT_MS_MIN : g_shot_ms - SHOT_MS_STEP;
            status();
            break;
        case ']':
            g_release_ms =
                (g_release_ms + RELEASE_MS_STEP > RELEASE_MS_MAX) ? RELEASE_MS_MAX : g_release_ms + RELEASE_MS_STEP;
            status();
            break;
        case '[':
            g_release_ms = (g_release_ms < RELEASE_MS_STEP) ? 0u : g_release_ms - RELEASE_MS_STEP;
            status();
            break;
        case 's': status(); break;
        case '?': case 'h': help(); break;
        case '\r': case '\n': break;
        default: ESP_LOGW(TAG, "unknown key 0x%02x -- ? for help", c); break;
        }
    }
}
