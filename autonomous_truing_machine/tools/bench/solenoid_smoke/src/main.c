/* Solenoid bench for both driver channels, driven by hand from the serial console.
 * LEFT = GPIO 9 (D6), RIGHT = GPIO 10 (D7) on the Nano ESP32 -- the pins the two-station
 * firmware declares as BOARD_PLUCK_ACTUATOR_LEFT/RIGHT_GPIO. Same gate wiring as pluck_gpio.c
 * (100-150 ohm series gate resistor, 10 k gate-source pulldown), none of the acoustic or
 * orchestrator code.
 *
 * The gate is driven through LEDC (PWM) instead of a plain GPIO write, so duty can ramp
 * instead of stepping straight between 0 and full. Both edges are adjustable and independent:
 *
 *   - "push ms": duty 0 -> full before the hold. 0 = instant full duty (the plunger's own
 *     speed is what limits how fast it reaches the spoke).
 *   - "release ms": duty full -> 0 after the hold. A push solenoid retracts on its own spring
 *     once current stops, and how fast that current collapses through the flyback path (hard
 *     cutoff vs. a duty ramp) is a real, previously uncontrolled variable in how sharp or soft
 *     the release looks and sounds. 0 = instant cutoff, the original behaviour.
 *
 * Nothing fires on boot. Every activation is one keystroke, so a single observation can be
 * repeated as often as it takes to be sure of it:
 *
 *   l / r    one shot on LEFT / RIGHT at the current shot width, push ramp and release ramp
 *   L / R    500 ms hold on LEFT / RIGHT (read V_DS on the meter during it)
 *   a        10 alternating shots, LEFT first, 3 s apart; any key aborts
 *   + / -    shot width +/- 5 ms (5 ms floor, no ceiling)
 *   { / }    push ramp -/+ 5 ms (0 ms floor = instant full duty; no ceiling)
 *   [ / ]    release ramp -/+ 5 ms (0 ms floor = instant cutoff; no ceiling)
 *   s        status: shot width, push ramp, release ramp, per-channel fire counts
 *   ? / h    this help
 *
 * Every pulse logs its channel, that channel's running count, the push/release ramps used and
 * the esp_timer-measured hold width. Keys typed while a pulse is out are discarded, so holding
 * a key down cannot queue a burst, and fires are spaced at least MIN_GAP_MS apart.
 *
 * Manual jog: a live duty knob, held indefinitely, independent of the timed push/hold/release
 * above -- for watching the actuator's own response as duty is walked up or down by hand, one
 * step at a time, rather than programming a ramp length and firing it. IMPORTANT: this steps
 * PWM duty, not an analog voltage. The gate itself is still switched hard between 0 V and
 * 3.3 V at LEDC_FREQ_HZ the whole time; there is no capacitor at the gate node (and the S3 has
 * no onboard DAC) to smooth that into a real DC level, so a meter on the gate would read a
 * fixed-amplitude square wave whose pulse width is changing, not a rising line. What DOES ramp
 * smoothly is the coil current -- the solenoid's own inductance integrates a switching period
 * far shorter than its electrical time constant into a genuinely smooth average -- which is
 * also the thing that sets how hard the plunger pulls. Every jog step logs the duty and the
 * average voltage that duty corresponds to (duty/FULL_DUTY * 3.3 V), labelled "avg" because
 * that is what it is -- an average, not a gate measurement.
 *
 *   c        select the channel jog acts on (announces the new selection, fires nothing)
 *   u / d    jog the selected channel's live duty up / down one step (0 floor, full-duty ceiling)
 *   x        kill -- force both channels' live duty to 0 immediately */
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
#define PUSH_MS_INIT    0u
#define PUSH_MS_STEP    5u
#define RELEASE_MS_INIT 0u
#define RELEASE_MS_STEP 5u
#define RUN_SHOTS       10
#define RUN_GAP_MS      3000u
#define MIN_GAP_MS      250u

#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_RES_BITS   LEDC_TIMER_10_BIT
#define LEDC_FULL_DUTY  ((1u << 10) - 1u)  /* matches LEDC_RES_BITS */
#define LEDC_FREQ_HZ    20000u             /* above audible range; far above coil bandwidth */
#define RAMP_STEPS      20u
#define GATE_MV         3300u              /* 3.3 V rail, in millivolts, for the avg-voltage log */
#define MANUAL_DUTY_STEP ((LEDC_FULL_DUTY * 5u) / 100u)  /* ~5% of full duty per jog step */

static const char *TAG = "solenoid_bench";

typedef struct {
    const char *name;
    int gpio;
    ledc_channel_t channel;
    unsigned fires;
    uint32_t live_duty;  /* current jog/hold duty, tracked so status() and 'c' can report it */
} channel_t;

static channel_t g_left = { "LEFT", LEFT_GPIO, LEDC_CHANNEL_0, 0u, 0u };
static channel_t g_right = { "RIGHT", RIGHT_GPIO, LEDC_CHANNEL_1, 0u, 0u };
static channel_t *g_manual_ch = &g_left;
static uint32_t g_shot_ms = SHOT_MS_INIT;
static uint32_t g_push_ms = PUSH_MS_INIT;
static uint32_t g_release_ms = RELEASE_MS_INIT;
static int64_t g_last_end_us;

static uint32_t mv_for_duty(uint32_t duty)
{
    return (uint32_t)(((uint64_t)duty * GATE_MV) / LEDC_FULL_DUTY);
}

static void flush_input(void)
{
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) > 0) {
    }
}

/* Linear duty ramp from from_duty to to_duty over ramp_ms, in RAMP_STEPS steps. ramp_ms == 0 is
 * a single instant step to to_duty -- the original (pre-ramp) behaviour on both edges. Returns
 * false the moment any underlying call fails, so a driver error can never read back as clean. */
static bool ramp_duty(channel_t *ch, uint32_t from_duty, uint32_t to_duty, uint32_t ramp_ms)
{
    if (ramp_ms == 0u) {
        return ledc_set_duty_and_update(LEDC_MODE, ch->channel, to_duty, 0) == ESP_OK;
    }
    /* Neither ramp has a ceiling, so widen before multiplying by 1000 -- ramp_ms alone would
     * already overflow a uint32_t microsecond count past roughly 71 minutes. */
    const uint64_t step_us64 = ((uint64_t)ramp_ms * 1000u) / RAMP_STEPS;
    const uint32_t step_us = (step_us64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)step_us64;
    const int32_t span = (int32_t)to_duty - (int32_t)from_duty;
    for (uint32_t i = 1; i <= RAMP_STEPS; i++) {
        const uint32_t duty = (i >= RAMP_STEPS) ? to_duty : (uint32_t)((int32_t)from_duty + (span * (int32_t)i) / (int32_t)RAMP_STEPS);
        if (ledc_set_duty_and_update(LEDC_MODE, ch->channel, duty, 0) != ESP_OK) {
            return false;
        }
        esp_rom_delay_us(step_us);
    }
    return true;
}

/* Sets and holds an absolute duty -- the manual-jog primitive. Unlike ramp_duty() it does not
 * return to 0 on its own; the duty stays exactly where it's set until the next jog, kill or
 * fire(). Checked and logged the same way as every other duty write, with the average voltage
 * that duty corresponds to (see the "Manual jog" note above the key list for what that is and
 * is not measuring). */
static bool set_live_duty(channel_t *ch, uint32_t new_duty)
{
    const bool ok = ledc_set_duty_and_update(LEDC_MODE, ch->channel, new_duty, 0) == ESP_OK;
    if (!ok) {
        ESP_LOGE(TAG, "%s (GPIO %d): jog FAILED at duty %u/%u -- gate may not be at the commanded level",
                  ch->name, ch->gpio, (unsigned)new_duty, (unsigned)LEDC_FULL_DUTY);
        return false;
    }
    ch->live_duty = new_duty;
    ESP_LOGI(TAG, "%s (GPIO %d): duty %u/%u, avg ~%u mV", ch->name, ch->gpio, (unsigned)new_duty,
             (unsigned)LEDC_FULL_DUTY, (unsigned)mv_for_duty(new_duty));
    return true;
}

/* Every step's return is checked. A driver failure is reported as a failed fire, never logged
 * as though the pulse went out -- an earlier revision silently counted these as fires because
 * ledc_set_duty_and_update()'s result went unchecked; the gate was never actually driven. */
static void fire(channel_t *ch, uint32_t pulse_ms, uint32_t push_ms, uint32_t release_ms)
{
    const int64_t since_ms = (esp_timer_get_time() - g_last_end_us) / 1000;
    if (since_ms < (int64_t)MIN_GAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(MIN_GAP_MS - (uint32_t)since_ms));
    }
    const int64_t t0 = esp_timer_get_time();
    const bool push_ok = ramp_duty(ch, 0, LEDC_FULL_DUTY, push_ms);
    const int64_t t_push_end = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(pulse_ms));
    const int64_t t_hold_end = esp_timer_get_time();
    const bool release_ok = ramp_duty(ch, LEDC_FULL_DUTY, 0, release_ms);
    g_last_end_us = esp_timer_get_time();
    /* A timed fire always intends to leave the gate at 0. On success that's exactly where
     * release_ok's last step put it; on failure the real hardware duty is whatever the last
     * successful write left behind, which live_duty can no longer promise to reflect -- 'x'
     * (kill) is the honest way back to a known state after a FAILED line. */
    ch->live_duty = 0u;
    if (!push_ok || !release_ok) {
        ESP_LOGE(TAG, "%s (GPIO %d): FAILED, pulse incomplete (push_ok=%d release_ok=%d) -- gate state is "
                  "not known, press x to force both channels to 0", ch->name, ch->gpio, (int)push_ok,
                 (int)release_ok);
        flush_input();
        return;
    }
    ch->fires++;
    ESP_LOGI(TAG, "%s #%u (GPIO %d): commanded %u ms push + %u ms hold + %u ms release, measured push %lld "
                  "us, hold %lld us, total %lld us", ch->name, ch->fires, ch->gpio, (unsigned)push_ms,
             (unsigned)pulse_ms, (unsigned)release_ms, (long long)(t_push_end - t0),
             (long long)(t_hold_end - t_push_end), (long long)(g_last_end_us - t0));
    flush_input();
}

static void help(void)
{
    ESP_LOGI(TAG, "keys: l/r shot LEFT/RIGHT | L/R 500 ms hold | a 10 alternating (any key aborts) | "
                  "+/- shot width | {/} push ramp | [/] release ramp | s status | "
                  "c/u/d/x manual jog (select/up/down/kill) | ? help");
}

static void status(void)
{
    ESP_LOGI(TAG, "shot width %u ms | push ramp %u ms (0 = instant full duty) | release ramp %u ms "
                  "(0 = instant cutoff) | LEFT fired %u, duty %u/%u | RIGHT fired %u, duty %u/%u | "
                  "jog selected: %s", (unsigned)g_shot_ms, (unsigned)g_push_ms, (unsigned)g_release_ms,
             g_left.fires, (unsigned)g_left.live_duty, (unsigned)LEDC_FULL_DUTY, g_right.fires,
             (unsigned)g_right.live_duty, (unsigned)LEDC_FULL_DUTY, g_manual_ch->name);
}

static void alternating_run(void)
{
    ESP_LOGI(TAG, "alternating run: %d shots at %u ms push + %u ms hold + %u ms release, %u ms apart, "
                  "LEFT first -- any key aborts", RUN_SHOTS, (unsigned)g_push_ms, (unsigned)g_shot_ms,
             (unsigned)g_release_ms, (unsigned)RUN_GAP_MS);
    for (int i = 1; i <= RUN_SHOTS; i++) {
        const bool left = (i % 2) == 1;
        ESP_LOGI(TAG, "run shot %d/%d -> %s", i, RUN_SHOTS, left ? "LEFT" : "RIGHT");
        fire(left ? &g_left : &g_right, g_shot_ms, g_push_ms, g_release_ms);
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

    /* ledc_set_duty_and_update() -- used for every duty change below, ramped or instant -- is
     * documented to require this once, first. Without it every call fails with "Fade service
     * not installed" and the gate is never actually driven; fire() now catches that failure
     * instead of logging a fire that didn't happen, but this is the actual fix. */
    ESP_ERROR_CHECK(ledc_fade_func_install(0));
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
        case 'l': fire(&g_left, g_shot_ms, g_push_ms, g_release_ms); break;
        case 'r': fire(&g_right, g_shot_ms, g_push_ms, g_release_ms); break;
        case 'L':
            ESP_LOGI(TAG, "LEFT hold -- read V_DS now");
            fire(&g_left, HOLD_PULSE_MS, g_push_ms, g_release_ms);
            break;
        case 'R':
            ESP_LOGI(TAG, "RIGHT hold -- read V_DS now");
            fire(&g_right, HOLD_PULSE_MS, g_push_ms, g_release_ms);
            break;
        case 'a': alternating_run(); break;
        /* No ceiling on any of the three knobs -- only enough floor/overflow guard that a run
         * of key repeats can't wrap a uint32_t or go negative. Hold time, ramp lengths and coil
         * duty cycle are the operator's call to make, not this sketch's. */
        case '+':
            g_shot_ms = (g_shot_ms > UINT32_MAX - SHOT_MS_STEP) ? UINT32_MAX : g_shot_ms + SHOT_MS_STEP;
            status();
            break;
        case '-':
            g_shot_ms = (g_shot_ms < SHOT_MS_MIN + SHOT_MS_STEP) ? SHOT_MS_MIN : g_shot_ms - SHOT_MS_STEP;
            status();
            break;
        case '}':
            g_push_ms = (g_push_ms > UINT32_MAX - PUSH_MS_STEP) ? UINT32_MAX : g_push_ms + PUSH_MS_STEP;
            status();
            break;
        case '{':
            g_push_ms = (g_push_ms < PUSH_MS_STEP) ? 0u : g_push_ms - PUSH_MS_STEP;
            status();
            break;
        case ']':
            g_release_ms =
                (g_release_ms > UINT32_MAX - RELEASE_MS_STEP) ? UINT32_MAX : g_release_ms + RELEASE_MS_STEP;
            status();
            break;
        case '[':
            g_release_ms = (g_release_ms < RELEASE_MS_STEP) ? 0u : g_release_ms - RELEASE_MS_STEP;
            status();
            break;
        /* Manual jog: a live, held duty, independent of the timed shot/push/release above.
         * See the header comment for what this is and is not driving at the gate. */
        case 'c':
            g_manual_ch = (g_manual_ch == &g_left) ? &g_right : &g_left;
            ESP_LOGI(TAG, "jog now selects %s (currently duty %u/%u)", g_manual_ch->name,
                     (unsigned)g_manual_ch->live_duty, (unsigned)LEDC_FULL_DUTY);
            break;
        case 'u': {
            const uint32_t next = (g_manual_ch->live_duty > LEDC_FULL_DUTY - MANUAL_DUTY_STEP)
                                       ? LEDC_FULL_DUTY
                                       : g_manual_ch->live_duty + MANUAL_DUTY_STEP;
            set_live_duty(g_manual_ch, next);
            break;
        }
        case 'd': {
            const uint32_t next =
                (g_manual_ch->live_duty < MANUAL_DUTY_STEP) ? 0u : g_manual_ch->live_duty - MANUAL_DUTY_STEP;
            set_live_duty(g_manual_ch, next);
            break;
        }
        case 'x':
            ESP_LOGW(TAG, "KILL -- forcing both channels to duty 0");
            set_live_duty(&g_left, 0u);
            set_live_duty(&g_right, 0u);
            break;
        case 's': status(); break;
        case '?': case 'h': help(); break;
        case '\r': case '\n': break;
        default: ESP_LOGW(TAG, "unknown key 0x%02x -- ? for help", c); break;
        }
    }
}
