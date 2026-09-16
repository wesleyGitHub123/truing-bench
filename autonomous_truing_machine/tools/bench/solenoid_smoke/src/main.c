/* Solenoid bench for both driver channels, driven by hand from the serial console.
 * LEFT = GPIO 9 (D6), RIGHT = GPIO 10 (D7) on the Nano ESP32 -- the pins the two-station
 * firmware declares as BOARD_PLUCK_ACTUATOR_LEFT/RIGHT_GPIO. Same gate wiring as pluck_gpio.c
 * (100-150 ohm series gate resistor, 10 k gate-source pulldown), none of the acoustic or
 * orchestrator code.
 *
 * The gate is driven through LEDC (PWM) instead of a plain GPIO write, so duty can ramp and be
 * held at intermediate levels. One shot runs five stages, every one of them adjustable:
 *
 *     push -> hold -> approach -> brake -> cutoff -> (catch delay) -> catch -> off
 *
 *   - "push ms": duty 0 -> full before the hold. 0 = instant full duty.
 *   - "hold ms" (shot width): time at full duty. L/R override it with HOLD_PULSE_MS.
 *   - "release ms": duty full -> brake duty. MEASURED: this only moves WHEN the plunger lets
 *     go, not how hard it lands. See the hysteresis note below.
 *   - "brake duty / brake ms": duty held after the approach, before the final cutoff. MEASURED
 *     INEFFECTIVE for softening the landing, for the reason below -- kept because it is still
 *     the mechanism for delaying release, and because 0/0 is the no-op default.
 *   - "catch duty / catch delay / catch width": a short, strong pulse fired AFTER cutoff, timed
 *     to land inside the retraction transient. This is the one aimed at impact noise.
 *
 * Bench-measured hysteresis (LEFT, this rig): the plunger extends progressively around duty
 * 765-867/1023 and does not drop out until roughly 153-204/1023 -- a 4-5x gap between pull-in
 * and drop-out, which is ordinary for a solenoid. Going up, each duty has a stable resting
 * position (1/4, half, full extension). Going down, there is none: magnetic force scales about
 * as 1/gap^2, so the instant the armature breaks free the gap opens, force collapses, the
 * spring wins harder, and it snaps home in one runaway motion.
 *
 * That runaway is why a constant brake duty cannot soften the landing. Any duty low enough to
 * let go is, by definition, too weak to resist once the gap opens. A catch pulse is different:
 * it fires at PULL-IN level, which is by definition a current that can move the plunger at a
 * large gap, so it can actually decelerate a plunger already in flight. The risk is the other
 * side of that coin -- too strong or too long and it does not slow the plunger, it re-extends
 * it, which on a real rig means a second strike on the spoke.
 *
 * Finding the transit time with no position sensor: set catch duty high and catch width wide,
 * start with a long catch delay (the retraction clack and the re-extension are two separate
 * events), then walk the delay down until the clack disappears into the catch. That delay is
 * roughly the transit time; sweep width and duty inside it from there.
 *
 * Nothing fires on boot. Every activation is one keystroke, so a single observation can be
 * repeated as often as it takes to be sure of it:
 *
 *   l / r    one shot on LEFT / RIGHT with the whole profile below
 *   b / v    baseline shot on LEFT / RIGHT -- push/release/brake/catch forced to 0 regardless
 *            of the tuned knobs below, so a tuned attempt can be A/B'd against plain
 *            push-hold-instant-cutoff without re-zeroing (and losing) whatever is dialled in
 *   L / R    same, but a HOLD_PULSE_MS hold (read V_DS on the meter during it)
 *   a        10 alternating shots, LEFT first, 3 s apart; any key aborts
 *   A        one-person V_DS check: alternating long holds (baseline profile), each with a
 *            lead-in announcement first so the meter goes on target BEFORE it fires -- unlike
 *            l/r/L/R this doesn't require you to already be on the right MOSFET when you press
 *            the key; any key aborts
 *   + / -    shot width +/- 5 ms (5 ms floor, no ceiling)
 *   { / }    push ramp -/+ 5 ms (0 ms floor = instant full duty; no ceiling)
 *   [ / ]    release ramp -/+ 5 ms (0 ms floor = instant; no ceiling)
 *   k / K    brake duty -/+ ~5% of full duty (0 floor, full-duty ceiling)
 *   m / M    brake hold -/+ 5 ms (0 ms floor = no brake stage; no ceiling)
 *   g / G    catch duty -/+ ~5% of full duty (0 floor, full-duty ceiling)
 *   t / T    catch delay -/+ 1 ms after cutoff (0 floor, no ceiling)
 *   w / W    catch width -/+ 1 ms (0 ms floor = no catch pulse at all; no ceiling)
 *   s        status: every knob above, plus per-channel fire counts
 *   ? / h    this help
 *
 * Catch delay and width step by 1 ms, not 5: the whole transient they have to land inside is
 * single- to low-double-digit milliseconds, and they are timed with esp_rom_delay_us rather
 * than vTaskDelay because the FreeRTOS tick is far too coarse for that window.
 *
 * Every pulse logs the profile it used and the esp_timer-measured time in each stage. Keys
 * typed while a pulse is out are discarded, so holding a key down cannot queue a burst, and
 * fires are spaced at least MIN_GAP_MS apart.
 *
 * Manual jog: a live duty knob, held indefinitely, independent of the timed shot above -- for
 * watching the actuator's own response as duty is walked up or down by hand, one step at a
 * time. This is how the hysteresis numbers above were measured. IMPORTANT: it steps PWM duty,
 * not an analog voltage. The gate is still switched hard between 0 V and 3.3 V at LEDC_FREQ_HZ
 * the whole time; there is no capacitor at the gate node (and the S3 has no onboard DAC) to
 * smooth that into a real DC level, so a meter on the gate would read a fixed-amplitude square
 * wave whose pulse width is changing, not a rising line. What DOES vary smoothly is the coil
 * current -- the solenoid's own inductance integrates a switching period far shorter than its
 * electrical time constant -- which is also the thing that sets how hard the plunger pulls.
 * Every jog step logs the duty and the average voltage that duty corresponds to, labelled
 * "avg" because that is what it is -- an average, not a gate measurement.
 *
 *   c        select the channel jog acts on (announces the new selection, fires nothing)
 *   u / d    jog the selected channel's live duty up / down one step (0 floor, full-duty ceiling)
 *   x        kill -- force both channels' live duty to 0 immediately. Clears live output only,
 *            never the profile knobs; a kill mid-experiment must not lose a tuned setting. */
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

#define LEFT_GPIO        9    /* D6 */
#define RIGHT_GPIO       10   /* D7 */
#define HOLD_PULSE_MS    500u
#define SHOT_MS_INIT     20u
#define SHOT_MS_STEP     5u
#define SHOT_MS_MIN      5u
#define PUSH_MS_INIT     0u
#define PUSH_MS_STEP     5u
#define RELEASE_MS_INIT  0u
#define RELEASE_MS_STEP  5u
#define BRAKE_DUTY_INIT  0u
#define BRAKE_MS_INIT    0u
#define BRAKE_MS_STEP    5u
#define CATCH_DUTY_INIT  0u
#define CATCH_DELAY_INIT 0u
#define CATCH_WIDTH_INIT 0u
#define CATCH_MS_STEP    1u   /* 1 ms: the transient this has to land inside is short */
#define RUN_SHOTS        10
#define RUN_GAP_MS       3000u
#define VDS_LEADIN_MS    4000u  /* time to get the meter on target before the hold fires */
#define VDS_HOLD_MS      4000u  /* long enough for a stable multimeter reading */
#define VDS_CYCLES       2      /* LEFT/RIGHT cycles -- 2 holds per channel */
#define MIN_GAP_MS       250u

#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_RES_BITS   LEDC_TIMER_10_BIT
#define LEDC_FULL_DUTY  ((1u << 10) - 1u)  /* matches LEDC_RES_BITS */
#define LEDC_FREQ_HZ    20000u             /* above audible range; far above coil bandwidth */
#define RAMP_STEPS      20u
#define GATE_MV         3300u              /* 3.3 V rail, in millivolts, for the avg-voltage log */
#define DUTY_STEP       ((LEDC_FULL_DUTY * 5u) / 100u)  /* ~5% of full duty, for every duty knob */

static const char *TAG = "solenoid_bench";

typedef struct {
    const char *name;
    int gpio;
    ledc_channel_t channel;
    unsigned fires;
    uint32_t live_duty;  /* current jog/hold duty, tracked so status() and 'c' can report it */
} channel_t;

/* One shot's whole profile. Grouped rather than passed positionally: eight uint32_t arguments
 * in a row is a silent argument-order bug waiting to happen, and every one of these is a knob
 * the operator sweeps independently. */
typedef struct {
    uint32_t hold_ms;
    uint32_t push_ms;
    uint32_t release_ms;
    uint32_t brake_duty;
    uint32_t brake_ms;
    uint32_t catch_duty;
    uint32_t catch_delay_ms;
    uint32_t catch_width_ms;
} shot_profile_t;

static channel_t g_left = { "LEFT", LEFT_GPIO, LEDC_CHANNEL_0, 0u, 0u };
static channel_t g_right = { "RIGHT", RIGHT_GPIO, LEDC_CHANNEL_1, 0u, 0u };
static channel_t *g_manual_ch = &g_left;
static shot_profile_t g_shot = {
    .hold_ms = SHOT_MS_INIT,
    .push_ms = PUSH_MS_INIT,
    .release_ms = RELEASE_MS_INIT,
    .brake_duty = BRAKE_DUTY_INIT,
    .brake_ms = BRAKE_MS_INIT,
    .catch_duty = CATCH_DUTY_INIT,
    .catch_delay_ms = CATCH_DELAY_INIT,
    .catch_width_ms = CATCH_WIDTH_INIT,
};
static int64_t g_last_end_us;

static uint32_t mv_for_duty(uint32_t duty)
{
    return (uint32_t)(((uint64_t)duty * GATE_MV) / LEDC_FULL_DUTY);
}

/* Busy-wait with microsecond resolution. The catch stages need it: the FreeRTOS tick is far
 * too coarse for a window measured in single-digit milliseconds. Chunked so that an unbounded
 * ms value cannot overflow esp_rom_delay_us's uint32_t microsecond argument. */
static void delay_ms_precise(uint32_t ms)
{
    while (ms > 0u) {
        const uint32_t chunk = (ms > 1000u) ? 1000u : ms;
        esp_rom_delay_us(chunk * 1000u);
        ms -= chunk;
    }
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
    /* No ramp has a ceiling, so widen before multiplying by 1000 -- ramp_ms alone would already
     * overflow a uint32_t microsecond count past roughly 71 minutes. */
    const uint64_t step_us64 = ((uint64_t)ramp_ms * 1000u) / RAMP_STEPS;
    const uint32_t step_us = (step_us64 > UINT32_MAX) ? UINT32_MAX : (uint32_t)step_us64;
    const int32_t span = (int32_t)to_duty - (int32_t)from_duty;
    for (uint32_t i = 1; i <= RAMP_STEPS; i++) {
        const uint32_t duty =
            (i >= RAMP_STEPS) ? to_duty
                              : (uint32_t)((int32_t)from_duty + (span * (int32_t)i) / (int32_t)RAMP_STEPS);
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

/* Every duty write's return is checked. A driver failure is reported as a failed fire, never
 * logged as though the pulse went out -- an earlier revision silently counted these as fires
 * because ledc_set_duty_and_update()'s result went unchecked; the gate was never driven.
 *
 * catch_width_ms == 0 skips the catch stage entirely, and brake_duty == brake_ms == 0 collapses
 * the release to a plain ramp-to-zero, so an all-zero profile is exactly the original
 * push/hold/cutoff behaviour. */
static void fire(channel_t *ch, const shot_profile_t *p)
{
    const int64_t since_ms = (esp_timer_get_time() - g_last_end_us) / 1000;
    if (since_ms < (int64_t)MIN_GAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(MIN_GAP_MS - (uint32_t)since_ms));
    }
    const int64_t t0 = esp_timer_get_time();
    const bool push_ok = ramp_duty(ch, 0, LEDC_FULL_DUTY, p->push_ms);
    const int64_t t_push_end = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(p->hold_ms));
    const int64_t t_hold_end = esp_timer_get_time();
    const bool approach_ok = ramp_duty(ch, LEDC_FULL_DUTY, p->brake_duty, p->release_ms);
    const int64_t t_approach_end = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(p->brake_ms));
    const bool cutoff_ok = ledc_set_duty_and_update(LEDC_MODE, ch->channel, 0, 0) == ESP_OK;
    const int64_t t_cutoff = esp_timer_get_time();

    /* The catch: a short pulse timed to land inside the retraction transient that starts at the
     * cutoff above. Both stages are busy-waited for microsecond accuracy. */
    bool catch_ok = true;
    int64_t t_catch_on = t_cutoff;
    int64_t t_catch_off = t_cutoff;
    if (p->catch_width_ms > 0u) {
        delay_ms_precise(p->catch_delay_ms);
        t_catch_on = esp_timer_get_time();
        catch_ok = ledc_set_duty_and_update(LEDC_MODE, ch->channel, p->catch_duty, 0) == ESP_OK;
        delay_ms_precise(p->catch_width_ms);
        catch_ok = catch_ok && (ledc_set_duty_and_update(LEDC_MODE, ch->channel, 0, 0) == ESP_OK);
        t_catch_off = esp_timer_get_time();
    }
    g_last_end_us = esp_timer_get_time();

    /* A timed fire always intends to leave the gate at 0. On success that's exactly where the
     * cutoff (or the catch's own trailing write) put it; on failure the real hardware duty is
     * whatever the last successful write left behind, which live_duty can no longer promise to
     * reflect -- 'x' (kill) is the honest way back to a known state after a FAILED line. */
    ch->live_duty = 0u;
    if (!push_ok || !approach_ok || !cutoff_ok || !catch_ok) {
        ESP_LOGE(TAG, "%s (GPIO %d): FAILED, pulse incomplete (push=%d approach=%d cutoff=%d catch=%d) -- "
                  "gate state is not known, press x to force both channels to 0", ch->name, ch->gpio,
                 (int)push_ok, (int)approach_ok, (int)cutoff_ok, (int)catch_ok);
        flush_input();
        return;
    }
    ch->fires++;
    ESP_LOGI(TAG, "%s #%u (GPIO %d): cmd push %u + hold %u + approach %u -> brake %u/%u for %u ms -> cutoff"
                  " -> +%u ms catch %u/%u for %u ms | meas push %lld, hold %lld, approach %lld, brake %lld, "
                  "catch delay %lld, catch %lld, total %lld us",
             ch->name, ch->fires, ch->gpio, (unsigned)p->push_ms, (unsigned)p->hold_ms,
             (unsigned)p->release_ms, (unsigned)p->brake_duty, (unsigned)LEDC_FULL_DUTY,
             (unsigned)p->brake_ms, (unsigned)p->catch_delay_ms, (unsigned)p->catch_duty,
             (unsigned)LEDC_FULL_DUTY, (unsigned)p->catch_width_ms, (long long)(t_push_end - t0),
             (long long)(t_hold_end - t_push_end), (long long)(t_approach_end - t_hold_end),
             (long long)(t_cutoff - t_approach_end), (long long)(t_catch_on - t_cutoff),
             (long long)(t_catch_off - t_catch_on), (long long)(g_last_end_us - t0));
    flush_input();
}

static void help(void)
{
    ESP_LOGI(TAG, "keys: l/r shot LEFT/RIGHT | b/v baseline (raw, ignores every knob below) | "
                  "L/R long hold | a 10 alternating | A one-person V_DS check (lead-in + long "
                  "holds) | +/- shot width | {/} push ramp | [/] release ramp | k/K brake duty | "
                  "m/M brake ms | g/G catch duty | t/T catch delay | w/W catch width | s status | "
                  "c/u/d/x manual jog (select/up/down/kill) | ? help");
}

static void status(void)
{
    ESP_LOGI(TAG, "shot width %u ms | push ramp %u ms | release ramp %u ms | brake %u/%u (avg ~%u mV) for "
                  "%u ms | catch %u/%u (avg ~%u mV) for %u ms at +%u ms after cutoff (width 0 = no catch) | "
                  "LEFT fired %u, duty %u/%u | RIGHT fired %u, duty %u/%u | jog selected: %s",
             (unsigned)g_shot.hold_ms, (unsigned)g_shot.push_ms, (unsigned)g_shot.release_ms,
             (unsigned)g_shot.brake_duty, (unsigned)LEDC_FULL_DUTY, (unsigned)mv_for_duty(g_shot.brake_duty),
             (unsigned)g_shot.brake_ms, (unsigned)g_shot.catch_duty, (unsigned)LEDC_FULL_DUTY,
             (unsigned)mv_for_duty(g_shot.catch_duty), (unsigned)g_shot.catch_width_ms,
             (unsigned)g_shot.catch_delay_ms, g_left.fires, (unsigned)g_left.live_duty,
             (unsigned)LEDC_FULL_DUTY, g_right.fires, (unsigned)g_right.live_duty,
             (unsigned)LEDC_FULL_DUTY, g_manual_ch->name);
}

/* Every ramp/brake/catch stage forced to 0 -- the plain push/hold/instant-cutoff behaviour --
 * with the given hold. Shared by fire_baseline() (A/B reference, keeps the tuned shot width) and
 * vds_check_run() (a long fixed hold for meter reads), so "what a clean shot looks like" is
 * defined in exactly one place. */
static shot_profile_t baseline_profile(uint32_t hold_ms)
{
    const shot_profile_t p = {
        .hold_ms = hold_ms,
        .push_ms = 0u,
        .release_ms = 0u,
        .brake_duty = 0u,
        .brake_ms = 0u,
        .catch_duty = 0u,
        .catch_delay_ms = 0u,
        .catch_width_ms = 0u,
    };
    return p;
}

/* A/B reference: fires with every ramp/brake/catch stage forced to 0 -- the plain, original
 * push/hold/instant-cutoff behaviour -- regardless of whatever the tuned knobs currently hold.
 * Shot width is kept as-is, so a baseline and a tuned attempt differ in exactly the stages being
 * tested, nothing else. Without this, comparing against "no extras" means re-zeroing every knob
 * and losing whatever was being dialled in. */
static void fire_baseline(channel_t *ch)
{
    const shot_profile_t p = baseline_profile(g_shot.hold_ms);
    ESP_LOGI(TAG, "BASELINE (raw push/hold/instant-cutoff, no ramps/brake/catch) -- %s", ch->name);
    fire(ch, &p);
}

static void alternating_run(void)
{
    ESP_LOGI(TAG, "alternating run: %d shots, %u ms apart, LEFT first -- any key aborts", RUN_SHOTS,
             (unsigned)RUN_GAP_MS);
    status();
    for (int i = 1; i <= RUN_SHOTS; i++) {
        const bool left = (i % 2) == 1;
        ESP_LOGI(TAG, "run shot %d/%d -> %s", i, RUN_SHOTS, left ? "LEFT" : "RIGHT");
        fire(left ? &g_left : &g_right, &g_shot);
        uint8_t c;
        if (i < RUN_SHOTS && usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(RUN_GAP_MS)) > 0) {
            ESP_LOGW(TAG, "run aborted after shot %d/%d", i, RUN_SHOTS);
            flush_input();
            return;
        }
    }
    ESP_LOGI(TAG, "alternating run done");
}

/* One-person V_DS check: alternating long holds, each with a lead-in announcement first. The
 * problem this solves: 'l'/'r'/'L'/'R' fire the instant the key is received, so by the time you
 * look up at the meter the window may already be over, and a short hold gives the display no
 * time to settle even if you don't miss it. Here the countdown happens BEFORE the hold, so the
 * probes go on the right MOSFET while it's still off, and the hold itself is long enough to read
 * a stable value once it's on. Runs the baseline profile -- push/release/brake/catch stages are
 * irrelevant to a steady-state hold, and forcing them off keeps a catch pulse from firing well
 * after the operator has already moved to the next channel. */
static void vds_check_run(void)
{
    const int total = VDS_CYCLES * 2;
    ESP_LOGI(TAG, "V_DS check: %d holds (%d LEFT/RIGHT cycles), %u ms lead-in then %u ms hold each -- "
                  "any key aborts", total, VDS_CYCLES, (unsigned)VDS_LEADIN_MS, (unsigned)VDS_HOLD_MS);
    for (int i = 1; i <= total; i++) {
        const bool left = (i % 2) == 1;
        channel_t *ch = left ? &g_left : &g_right;
        ESP_LOGI(TAG, "%d/%d: get the meter on %s (GPIO %d) now -- hold starts in %u ms", i, total, ch->name,
                 ch->gpio, (unsigned)VDS_LEADIN_MS);
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(VDS_LEADIN_MS)) > 0) {
            ESP_LOGW(TAG, "V_DS check aborted during lead-in, %d/%d", i, total);
            flush_input();
            return;
        }
        ESP_LOGI(TAG, "%s (GPIO %d) holding now (%u ms) -- read V_DS", ch->name, ch->gpio,
                 (unsigned)VDS_HOLD_MS);
        const shot_profile_t p = baseline_profile(VDS_HOLD_MS);
        fire(ch, &p);
        if (i < total) {
            ESP_LOGI(TAG, "hold done -- next lead-in starts now");
        }
    }
    ESP_LOGI(TAG, "V_DS check done");
}

/* Duty knobs are clamped at full duty -- there is no duty above full. Time knobs have no
 * ceiling, only enough overflow guard that a run of key repeats can't wrap a uint32_t. Hold
 * times and coil duty cycle are the operator's call to make, not this sketch's. */
static uint32_t duty_up(uint32_t v)
{
    return (v > LEDC_FULL_DUTY - DUTY_STEP) ? LEDC_FULL_DUTY : v + DUTY_STEP;
}

static uint32_t duty_down(uint32_t v)
{
    return (v < DUTY_STEP) ? 0u : v - DUTY_STEP;
}

static uint32_t ms_up(uint32_t v, uint32_t step)
{
    return (v > UINT32_MAX - step) ? UINT32_MAX : v + step;
}

static uint32_t ms_down(uint32_t v, uint32_t step, uint32_t floor_ms)
{
    return (v < floor_ms + step) ? floor_ms : v - step;
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
        case 'l': fire(&g_left, &g_shot); break;
        case 'r': fire(&g_right, &g_shot); break;
        case 'b': fire_baseline(&g_left); break;
        case 'v': fire_baseline(&g_right); break;
        case 'L': {
            ESP_LOGI(TAG, "LEFT hold -- read V_DS now");
            shot_profile_t p = g_shot;
            p.hold_ms = HOLD_PULSE_MS;
            fire(&g_left, &p);
            break;
        }
        case 'R': {
            ESP_LOGI(TAG, "RIGHT hold -- read V_DS now");
            shot_profile_t p = g_shot;
            p.hold_ms = HOLD_PULSE_MS;
            fire(&g_right, &p);
            break;
        }
        case 'a': alternating_run(); break;
        case 'A': vds_check_run(); break;
        case '+': g_shot.hold_ms = ms_up(g_shot.hold_ms, SHOT_MS_STEP); status(); break;
        case '-': g_shot.hold_ms = ms_down(g_shot.hold_ms, SHOT_MS_STEP, SHOT_MS_MIN); status(); break;
        case '}': g_shot.push_ms = ms_up(g_shot.push_ms, PUSH_MS_STEP); status(); break;
        case '{': g_shot.push_ms = ms_down(g_shot.push_ms, PUSH_MS_STEP, 0u); status(); break;
        case ']': g_shot.release_ms = ms_up(g_shot.release_ms, RELEASE_MS_STEP); status(); break;
        case '[': g_shot.release_ms = ms_down(g_shot.release_ms, RELEASE_MS_STEP, 0u); status(); break;
        case 'K': g_shot.brake_duty = duty_up(g_shot.brake_duty); status(); break;
        case 'k': g_shot.brake_duty = duty_down(g_shot.brake_duty); status(); break;
        case 'M': g_shot.brake_ms = ms_up(g_shot.brake_ms, BRAKE_MS_STEP); status(); break;
        case 'm': g_shot.brake_ms = ms_down(g_shot.brake_ms, BRAKE_MS_STEP, 0u); status(); break;
        /* The catch stage: strong enough to move a plunger at a large gap, short enough not to
         * re-extend it. Both of those are bench questions, so both are knobs. */
        case 'G': g_shot.catch_duty = duty_up(g_shot.catch_duty); status(); break;
        case 'g': g_shot.catch_duty = duty_down(g_shot.catch_duty); status(); break;
        case 'T': g_shot.catch_delay_ms = ms_up(g_shot.catch_delay_ms, CATCH_MS_STEP); status(); break;
        case 't': g_shot.catch_delay_ms = ms_down(g_shot.catch_delay_ms, CATCH_MS_STEP, 0u); status(); break;
        case 'W': g_shot.catch_width_ms = ms_up(g_shot.catch_width_ms, CATCH_MS_STEP); status(); break;
        case 'w': g_shot.catch_width_ms = ms_down(g_shot.catch_width_ms, CATCH_MS_STEP, 0u); status(); break;
        /* Manual jog: a live, held duty, independent of the timed shot above.
         * See the header comment for what this is and is not driving at the gate. */
        case 'c':
            g_manual_ch = (g_manual_ch == &g_left) ? &g_right : &g_left;
            ESP_LOGI(TAG, "jog now selects %s (currently duty %u/%u)", g_manual_ch->name,
                     (unsigned)g_manual_ch->live_duty, (unsigned)LEDC_FULL_DUTY);
            break;
        case 'u': set_live_duty(g_manual_ch, duty_up(g_manual_ch->live_duty)); break;
        case 'd': set_live_duty(g_manual_ch, duty_down(g_manual_ch->live_duty)); break;
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
