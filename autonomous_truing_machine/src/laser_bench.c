/* Bench laser-stop glue (see laser_bench.h): the receiver's GPIO interrupt and a 2 ms poll feed the pure
 * trigger in truing_hal/beam_stop.c under one spinlock; this file is the only one that calls the drive's
 * stop() for it. Compiled into every image (src/ is globbed); it does something only on the lab image. */
#include "laser_bench.h"

#include "build_mode.h"

#if TRUING_BENCH_DRIVE

#include <string.h>

#include "board/board_profile.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "laser_bench";

#define BEAM_PIN        ((gpio_num_t)BOARD_SPOKE_BEAM_GPIO)
#define POLL_MS         2u      /* end-of-move resolution and the missed-edge check; the trigger itself is the interrupt */
#define PROBE_SETTLE_US 30u     /* the internal pull (~45 kohm) against a floating pin's few pF settles in well under 1 us */
#define PROBE_QUIET_US  1000u   /* interrupts during and just after a probe are the pull toggling, not the beam */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    truing_beam_stop_t                     bs;
    truing_beam_align_t                    al;   /* the alignment campaign's controller (beam_align.h) */
    truing_wheel_drive_if_t               *drive;
    const truing_wheel_drive_tmc2209_ctx_t *dctx;
    bool                                   ready;
    truing_laser_input_t                   input;
    uint32_t                               probed_at_us;
    int64_t                                quiet_until_us;   /* 64-bit board time; read and written under s_lock */
    uint8_t                                mismatch_polls;
} L;

static inline bool beam_blocked(void)
{
    return gpio_get_level(BEAM_PIN) == BOARD_SPOKE_BEAM_BLOCKED_LEVEL;
}

/* Call under s_lock. 64-bit, so it never wraps into a false "quiet" stretch. */
static inline bool in_quiet_window(int64_t now_us)
{
    return now_us < L.quiet_until_us;
}

/* Any edge on the receiver. Reads the counter and the level right here, so the trigger's step count is the
 * one at the interrupt -- the poll only finds the end of the move. stop() is the drive's own controlled
 * deceleration (step_ramp.h: "any context"); the drive-interface calls are plain reads and a flag set. */
static void beam_isr(void *arg)
{
    (void)arg;
    const bool blocked = beam_blocked();
    const int64_t now = esp_timer_get_time();
    const uint32_t t = (uint32_t)now;
    const bool moving = truing_wheel_drive_is_moving(L.drive);
    const int32_t pos = truing_wheel_drive_position_steps(L.drive);
    bool stop = false;
    portENTER_CRITICAL_ISR(&s_lock);
    if (in_quiet_window(now)) {
        L.bs.blocked = blocked;   /* a probe's pull toggle: follow the level, count nothing */
        L.al.blocked = blocked;
    } else {
        stop = truing_beam_stop_on_edge(&L.bs, blocked, t, pos, moving);
        /* The alignment controller sees the same edge; at most one of the two is ever active. */
        stop = truing_beam_align_on_edge(&L.al, blocked, t, pos, moving) || stop;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    if (stop) {
        truing_wheel_drive_stop(L.drive);
    }
}

/* Execute an alignment-controller command, outside the lock: a ramp change and/or a move through the drive
 * interface, exactly as `roll speed` / `roll <steps>` would. A refusal ends the run (FAULT, DRIVE_REFUSED). */
static void run_align_cmd(const truing_align_cmd_t *cmd)
{
    truing_reason_t why = TRUING_REASON_NONE;
    bool ok = true;
    if (cmd->set_motion) {
        ok = truing_wheel_drive_set_motion(L.drive, cmd->hz, cmd->accel, &why) == TRUING_STATUS_VALID;
        if (!ok) {
            ESP_LOGW(TAG, "align: ramp change to %lu steps/s, %lu steps/s2 refused", (unsigned long)cmd->hz,
                     (unsigned long)cmd->accel);
        }
    }
    if (ok && cmd->move) {
        ok = truing_wheel_drive_move_relative(L.drive, cmd->steps, &why) == TRUING_STATUS_VALID;
    }
    if (!ok && (cmd->move || cmd->set_motion)) {
        const int32_t pos = truing_wheel_drive_position_steps(L.drive);
        portENTER_CRITICAL(&s_lock);
        truing_beam_align_command_failed(&L.al, (uint32_t)esp_timer_get_time(), pos);
        portEXIT_CRITICAL(&s_lock);
    }
}

static void log_align_end(void)
{
    static truing_beam_align_t a;   /* the poll task's own copy: too big for its stack */
    portENTER_CRITICAL(&s_lock);
    memcpy(&a, &L.al, sizeof(a));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "align run %lu: %s%s%s -- %s, %u breaks, trigger %ld, stop %ld, final %ld, zone %d, beam %s",
             (unsigned long)a.run, truing_align_result_str(a.result), a.detail ? " / " : "",
             truing_align_detail_str(a.detail), truing_align_strategy_str(a.cfg.strategy), (unsigned)a.breaks,
             (long)a.trigger_pos, (long)a.stop_pos, (long)a.final_pos, (int)a.final_zone,
             a.final_blocked ? "BLOCKED" : "CLEAR");
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const bool blocked = beam_blocked();
        const int64_t now = esp_timer_get_time();
        const uint32_t t = (uint32_t)now;
        const bool moving = truing_wheel_drive_is_moving(L.drive);
        const int32_t pos = truing_wheel_drive_position_steps(L.drive);
        bool stop = false, ended = false;
        truing_beam_trial_t done = { 0 };
        portENTER_CRITICAL(&s_lock);
        /* An edge no interrupt reported (two pulses closer than its latency): only after two polls in a row
         * disagree, so an interrupt still in flight is never pre-empted by a poll that read the same edge. */
        if (!in_quiet_window(now) && blocked != L.bs.blocked) {
            if (++L.mismatch_polls >= 2u) {
                stop = truing_beam_stop_resync(&L.bs, blocked, t, pos, moving);
                stop = truing_beam_align_on_edge(&L.al, blocked, t, pos, moving) || stop;
                L.mismatch_polls = 0u;
            }
        } else {
            L.mismatch_polls = 0u;
        }
        ended = truing_beam_stop_poll(&L.bs, moving, pos, t, L.dctx->enabled, L.dctx->faulted,
                                      L.dctx->profile.max_step_hz, L.dctx->profile.accel_steps_s2);
        if (ended) {
            done = L.bs.last;
        }
        truing_align_cmd_t cmd;
        const bool al_done = truing_beam_align_tick(&L.al, t, pos, moving, L.dctx->enabled, L.dctx->faulted, &cmd);
        portEXIT_CRITICAL(&s_lock);
        if (stop) {
            truing_wheel_drive_stop(L.drive);
        }
        run_align_cmd(&cmd);
        if (al_done) {
            log_align_end();
        }
        if (ended) {
            if (done.fired) {
                ESP_LOGI(TAG, "laser-stop trial %lu: %s -- break at step %ld, last pulse at %ld (%+ld), %lu ms, "
                              "%lu steps/s %lu steps/s2",
                         (unsigned long)done.trial, truing_beam_result_str(done.result), (long)done.trigger_pos,
                         (long)done.final_pos, (long)(done.final_pos - done.trigger_pos),
                         (unsigned long)((done.end_t_us - done.trigger_t_us) / 1000u), (unsigned long)done.max_step_hz,
                         (unsigned long)done.accel_steps_s2);
            } else {
                ESP_LOGI(TAG, "laser-stop trial %lu: %s (the move ended at step %ld without a beam break)",
                         (unsigned long)done.trial, truing_beam_result_str(done.result), (long)done.final_pos);
            }
        }
    }
}

/* Is anything driving the pin? Read it once with the internal pull-up and once with the pull-down: a pin
 * that follows the pull is floating. Only meaningful while the external network is much stiffer than the
 * ~45 kohm internal pulls. With BOARD_SPOKE_BEAM_RESTING_PULL 0 the divider (50k + 100k) is NOT stiffer, so
 * a healthy pin would read differently under each pull and the test would cry "floating" at a working
 * receiver: it is not run, and the input stays UNPROBED. */
static truing_laser_input_t probe_input(void)
{
#if BOARD_SPOKE_BEAM_RESTING_PULL
    portENTER_CRITICAL(&s_lock);
    L.quiet_until_us = esp_timer_get_time() + PROBE_QUIET_US;
    portEXIT_CRITICAL(&s_lock);
    gpio_set_pull_mode(BEAM_PIN, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(PROBE_SETTLE_US);
    const int up = gpio_get_level(BEAM_PIN);
    gpio_set_pull_mode(BEAM_PIN, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(PROBE_SETTLE_US);
    const int down = gpio_get_level(BEAM_PIN);
    gpio_set_pull_mode(BEAM_PIN, GPIO_PULLUP_ONLY);   /* the resting pull: an unwired pin reads high = "no laser" */
    L.probed_at_us = (uint32_t)esp_timer_get_time();
    return up != down ? TRUING_LASER_INPUT_FLOATING : TRUING_LASER_INPUT_DRIVEN;
#else
    return TRUING_LASER_INPUT_UNPROBED;
#endif
}

bool truing_laser_bench_init(truing_wheel_drive_if_t *drive, const truing_wheel_drive_tmc2209_ctx_t *dctx)
{
    if (L.ready || drive == NULL || dctx == NULL) {
        return L.ready;
    }
    L.drive = drive;   /* before the interrupt can fire */
    L.dctx = dctx;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_SPOKE_BEAM_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* No internal pull by default: it would fight the divider's 100 kohm bottom leg (board_s3_devkit.h). */
        .pull_up_en = BOARD_SPOKE_BEAM_RESTING_PULL ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t e = gpio_config(&io);
    if (e == ESP_OK) {
        e = gpio_install_isr_service(0);   /* non-IRAM: held off while flash writes disable the cache */
        if (e == ESP_ERR_INVALID_STATE) {
            e = ESP_OK;   /* already installed by someone else */
        }
    }
    truing_beam_stop_init(&L.bs, beam_blocked());
    truing_beam_align_init(&L.al, beam_blocked());
    if (e == ESP_OK) {
        e = gpio_isr_handler_add(BEAM_PIN, beam_isr, NULL);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "laser receiver input on GPIO %d not set up (%s): laser-stop unavailable", BOARD_SPOKE_BEAM_GPIO,
                 esp_err_to_name(e));
        return false;
    }
    L.input = probe_input();
    if (xTaskCreatePinnedToCore(poll_task, "laser_bench", 3072, NULL, tskIDLE_PRIORITY + 4, NULL, 0) != pdPASS) {
        gpio_isr_handler_remove(BEAM_PIN);
        ESP_LOGE(TAG, "laser poll task not created: laser-stop unavailable");
        return false;
    }
    L.ready = true;
    ESP_LOGI(TAG, "laser receiver on GPIO %d: input %s, beam %s; laser-stop DISARMED (GET /debug/laser.json)",
             BOARD_SPOKE_BEAM_GPIO,
             L.input == TRUING_LASER_INPUT_FLOATING ? "FLOATING (nothing connected?)"
             : L.input == TRUING_LASER_INPUT_UNPROBED ? "not pull-tested (high-impedance divider)" : "driven",
             beam_blocked() ? "BLOCKED / no laser seen" : "CLEAR (laser seen)");
    return true;
}

bool truing_laser_bench_snapshot(truing_beam_stop_t *out, truing_laser_bench_io_t *io)
{
    if (!L.ready) {
        return false;
    }
    if (out != NULL) {
        portENTER_CRITICAL(&s_lock);
        memcpy(out, &L.bs, sizeof(*out));
        portEXIT_CRITICAL(&s_lock);
    }
    if (io != NULL) {
        const uint32_t now = (uint32_t)esp_timer_get_time();
        io->ready = true;
        io->gpio = BOARD_SPOKE_BEAM_GPIO;
        io->level = gpio_get_level(BEAM_PIN);
        io->input = L.input;
        io->probed_age_ms = (now - L.probed_at_us) / 1000u;
        io->now_us = now;
    }
    return true;
}

bool truing_laser_bench_arm(bool on, const char **detail)
{
    const char *d = on ? "armed: the next beam break while the roller moves stops it" : "disarmed";
    bool ok = L.ready;
    if (!ok) {
        d = "no laser receiver input in this image";
    } else {
        const int32_t pos = truing_wheel_drive_position_steps(L.drive);
        bool aligning = false;
        portENTER_CRITICAL(&s_lock);
        if (on && truing_beam_align_active(&L.al)) {
            aligning = true;   /* two owners of the stop decision: refuse */
            ok = false;
        } else if (on) {
            ok = truing_beam_stop_arm(&L.bs, pos);
        } else {
            truing_beam_stop_disarm(&L.bs);
        }
        portEXIT_CRITICAL(&s_lock);
        if (!ok) {
            d = aligning ? "an alignment run is in progress: `align abort` first"
                         : "a triggered stop is still decelerating: arm again once it has ended";
        }
    }
    if (detail != NULL) {
        *detail = d;
    }
    return ok;
}

bool truing_laser_bench_probe(const char **detail)
{
    const char *d = "probed";
    bool ok = L.ready;
    if (!ok) {
        d = "no laser receiver input in this image";
    } else {
        portENTER_CRITICAL(&s_lock);
        const bool armed = L.bs.armed || (L.bs.cur.fired && !L.bs.cur.ended) || truing_beam_align_active(&L.al);
        portEXIT_CRITICAL(&s_lock);
        if (!BOARD_SPOKE_BEAM_RESTING_PULL) {
            ok = false;
            d = "no pull test with this divider (50k + 100k is not stiffer than the internal pulls): "
                "check the wiring with a card in the beam";
        } else if (armed || truing_wheel_drive_is_moving(L.drive)) {
            ok = false;
            d = "not while armed or moving: a pull toggle on a floating pin would look like a beam edge";
        } else {
            L.input = probe_input();
        }
    }
    if (detail != NULL) {
        *detail = d;
    }
    return ok;
}

bool truing_laser_align_start(const truing_align_config_t *cfg, const char **detail)
{
    const char *d = "started";
    if (!L.ready) {
        d = "no laser receiver input in this image";
    } else if (!L.dctx->enabled) {
        d = "the roller is off: `roll on` first";
    } else if (L.dctx->faulted) {
        d = "the drive has a latched fault: `roll on` clears it";
    } else {
        truing_align_cmd_t cmd;
        const bool moving = truing_wheel_drive_is_moving(L.drive);
        const int32_t pos = truing_wheel_drive_position_steps(L.drive);
        const bool blocked = beam_blocked();
        bool ok;
        portENTER_CRITICAL(&s_lock);
        if (L.bs.armed || (L.bs.cur.fired && !L.bs.cur.ended)) {
            ok = false;
            d = "the one-shot laser-stop is armed: `laser disarm` first";
        } else {
            ok = truing_beam_align_start(&L.al, cfg, pos, moving, blocked, (uint32_t)esp_timer_get_time(),
                                         L.dctx->profile.max_step_hz, L.dctx->profile.accel_steps_s2, &cmd, &d);
        }
        portEXIT_CRITICAL(&s_lock);
        if (ok) {
            run_align_cmd(&cmd);
            portENTER_CRITICAL(&s_lock);
            ok = !(L.al.result == TRUING_ALIGN_RESULT_FAULT && L.al.detail == TRUING_ALIGN_DETAIL_DRIVE_REFUSED);
            portEXIT_CRITICAL(&s_lock);
            if (!ok) {
                d = "the drive refused the approach move";
            } else {
                ESP_LOGI(TAG, "align run %lu started: %s, %s, dir %+d, stop on break %u, max %lu steps at %lu steps/s",
                         (unsigned long)L.al.run, cfg->mode == TRUING_ALIGN_MODE_COUNT ? "COUNT" : "ALIGN",
                         truing_align_strategy_str(cfg->strategy), (int)cfg->dir, (unsigned)cfg->target_break,
                         (unsigned long)cfg->max_steps, (unsigned long)L.dctx->profile.max_step_hz);
            }
        }
        if (detail != NULL) {
            *detail = d;
        }
        return ok;
    }
    if (detail != NULL) {
        *detail = d;
    }
    return false;
}

bool truing_laser_align_abort(const char **detail)
{
    if (!L.ready) {
        if (detail != NULL) {
            *detail = "no laser receiver input in this image";
        }
        return false;
    }
    const int32_t pos = truing_wheel_drive_position_steps(L.drive);
    portENTER_CRITICAL(&s_lock);
    const bool was = truing_beam_align_abort(&L.al, (uint32_t)esp_timer_get_time(), pos);
    portEXIT_CRITICAL(&s_lock);
    if (was) {
        truing_wheel_drive_stop(L.drive);   /* the same controlled stop `roll stop` asks for */
    }
    if (detail != NULL) {
        *detail = was ? "aborted: controlled stop requested" : "no run in progress";
    }
    return was;
}

bool truing_laser_align_snapshot(truing_beam_align_t *out)
{
    if (!L.ready || out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(out, &L.al, sizeof(*out));
    portEXIT_CRITICAL(&s_lock);
    return true;
}

#else /* !TRUING_BENCH_DRIVE: no bench drive, so no laser-stop */

bool truing_laser_align_start(const truing_align_config_t *cfg, const char **detail)
{
    (void)cfg;
    if (detail != NULL) {
        *detail = "no laser receiver input in this image";
    }
    return false;
}

bool truing_laser_align_abort(const char **detail)
{
    if (detail != NULL) {
        *detail = "no laser receiver input in this image";
    }
    return false;
}

bool truing_laser_align_snapshot(truing_beam_align_t *out)
{
    (void)out;
    return false;
}

bool truing_laser_bench_init(truing_wheel_drive_if_t *drive, const truing_wheel_drive_tmc2209_ctx_t *dctx)
{
    (void)drive;
    (void)dctx;
    return false;
}

bool truing_laser_bench_snapshot(truing_beam_stop_t *out, truing_laser_bench_io_t *io)
{
    (void)out;
    (void)io;
    return false;
}

bool truing_laser_bench_arm(bool on, const char **detail)
{
    (void)on;
    if (detail != NULL) {
        *detail = "no laser receiver input in this image";
    }
    return false;
}

bool truing_laser_bench_probe(const char **detail)
{
    if (detail != NULL) {
        *detail = "no laser receiver input in this image";
    }
    return false;
}

#endif
