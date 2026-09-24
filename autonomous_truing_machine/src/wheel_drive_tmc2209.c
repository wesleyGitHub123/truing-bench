/* TMC2209 wheel drive (see wheel_drive_tmc2209.h). */
#include "wheel_drive_tmc2209.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "truing_hal/tmc2209_frame.h"

static const char *TAG = "wheel_drive";

#define TICK_HZ       1000000u   /* the gptimer resolution every profile is expressed in */
#define STEP_HIGH_US  2u         /* TMC2209 needs >= 100 ns; 2 us is generous and cheap */
#define DIR_SETUP_US  5u         /* DIR must settle before the first STEP edge (>= 20 ns) */
#define UART_BAUD     115200

/* ---- STEP pulse train: one gptimer alarm per pulse ----------------------------------------- */
/* ISR context. The ramp arithmetic is integer-only (step_ramp.h). */
static bool step_alarm_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    truing_wheel_drive_tmc2209_ctx_t *c = (truing_wheel_drive_tmc2209_ctx_t *)user_ctx;
    if (!c->moving) {
        gptimer_stop(timer);
        return false;
    }
    gpio_set_level((gpio_num_t)c->pins.step_gpio, 1);
    esp_rom_delay_us(STEP_HIGH_US);
    gpio_set_level((gpio_num_t)c->pins.step_gpio, 0);
    c->position += c->dir_sign;   /* counted as emitted: this is the odometry */
    const uint32_t next = truing_ramp_step(&c->ramp);
    if (next == 0u) {
        c->moving = false;
        gptimer_stop(timer);
        return false;
    }
    const gptimer_alarm_config_t alarm = {
        .alarm_count = edata->alarm_value + next,
        .flags.auto_reload_on_alarm = false,
    };
    gptimer_set_alarm_action(timer, &alarm);
    return false;
}

static void halt_pulses(truing_wheel_drive_tmc2209_ctx_t *c)
{
    if (!c->moving) {
        return;   /* the ISR already stopped the timer; stopping it again only logs an error */
    }
    c->moving = false;
    if (c->timer != NULL) {
        gptimer_stop(c->timer);
    }
}

/* ---- single-wire UART ------------------------------------------------------------------------ */
/* Send `tx`, discard the echo, and (when `reply` is non-NULL) return the 8-byte reply. The echo
 * and the reply are read together and the reply taken from the tail, so a wiring that does not
 * echo still works -- it just waits out the timeout. */
static bool uart_xfer(truing_wheel_drive_tmc2209_ctx_t *c, const uint8_t *tx, size_t n_tx, uint8_t *reply)
{
    if (c->pins.uart_num < 0) {
        return false;
    }
    const uart_port_t port = (uart_port_t)c->pins.uart_num;
    uart_flush_input(port);
    if (uart_write_bytes(port, tx, n_tx) != (int)n_tx) {
        return false;
    }
    uart_wait_tx_done(port, pdMS_TO_TICKS(20));
    uint8_t buf[TRUING_TMC2209_WRITE_LEN + TRUING_TMC2209_REPLY_LEN];
    const size_t want = n_tx + (reply != NULL ? TRUING_TMC2209_REPLY_LEN : 0u);
    const int got = uart_read_bytes(port, buf, want, pdMS_TO_TICKS(reply != NULL ? 30 : 5));
    if (reply == NULL) {
        return true;
    }
    if (got < (int)TRUING_TMC2209_REPLY_LEN) {
        return false;
    }
    memcpy(reply, buf + got - TRUING_TMC2209_REPLY_LEN, TRUING_TMC2209_REPLY_LEN);
    return true;
}

bool truing_wheel_drive_tmc2209_read(truing_wheel_drive_tmc2209_ctx_t *ctx, uint8_t reg, uint32_t *value)
{
    if (ctx == NULL || ctx->pins.uart_num < 0) {
        return false;
    }
    uint8_t req[TRUING_TMC2209_READ_REQ_LEN];
    uint8_t rep[TRUING_TMC2209_REPLY_LEN];
    truing_tmc2209_read_request(req, ctx->pins.node, reg);
    return uart_xfer(ctx, req, sizeof(req), rep) && truing_tmc2209_parse_reply(rep, reg, value);
}

bool truing_wheel_drive_tmc2209_write(truing_wheel_drive_tmc2209_ctx_t *ctx, uint8_t reg, uint32_t value)
{
    if (ctx == NULL || ctx->pins.uart_num < 0) {
        return false;
    }
    uint8_t w[TRUING_TMC2209_WRITE_LEN];
    truing_tmc2209_write_frame(w, ctx->pins.node, reg, value);
    return uart_xfer(ctx, w, sizeof(w), NULL);
}

bool truing_wheel_drive_tmc2209_set_microsteps(truing_wheel_drive_tmc2209_ctx_t *ctx, unsigned microsteps)
{
    if (ctx == NULL || ctx->moving) {
        return false;
    }
    uint32_t gconf = 0u, chop = 0u, want = 0u, ifcnt0 = 0u, ifcnt1 = 0u, back = 0u;
    if (!truing_wheel_drive_tmc2209_read(ctx, TRUING_TMC2209_GCONF, &gconf) ||
        !truing_wheel_drive_tmc2209_read(ctx, TRUING_TMC2209_CHOPCONF, &chop) ||
        !truing_tmc2209_chopconf_set_microsteps(chop, microsteps, &want) ||
        !truing_wheel_drive_tmc2209_read(ctx, TRUING_TMC2209_IFCNT, &ifcnt0)) {
        return false;
    }
    /* pdn_disable: the PDN_UART pin is the UART, not the standstill-current input.
     * mstep_reg_select: take the microstep count from MRES, not the MS1/MS2 pins. */
    gconf |= TRUING_TMC2209_GCONF_PDN_DISABLE | TRUING_TMC2209_GCONF_MSTEP_REG_SELECT;
    if (!truing_wheel_drive_tmc2209_write(ctx, TRUING_TMC2209_GCONF, gconf) ||
        !truing_wheel_drive_tmc2209_write(ctx, TRUING_TMC2209_CHOPCONF, want) ||
        !truing_wheel_drive_tmc2209_read(ctx, TRUING_TMC2209_IFCNT, &ifcnt1) ||
        !truing_wheel_drive_tmc2209_read(ctx, TRUING_TMC2209_CHOPCONF, &back) ||
        !truing_wheel_drive_tmc2209_read(ctx, TRUING_TMC2209_GCONF, &gconf)) {
        return false;
    }
    ctx->gconf = gconf;
    ctx->chopconf = back;
    /* Both writes must have landed (IFCNT counts accepted writes, mod 256) and read back. */
    const bool ok = ((ifcnt1 - ifcnt0) & 0xFFu) == 2u && truing_tmc2209_chopconf_microsteps(back) == microsteps &&
                    (gconf & TRUING_TMC2209_GCONF_MSTEP_REG_SELECT) != 0u;
    ctx->microsteps_verified = ok ? microsteps : 0u;
    return ok;
}

bool truing_wheel_drive_tmc2209_diag_level(const truing_wheel_drive_tmc2209_ctx_t *ctx)
{
    return ctx != NULL && ctx->pins.diag_gpio >= 0 && gpio_get_level((gpio_num_t)ctx->pins.diag_gpio) != 0;
}

/* ---- wheel_drive_if ------------------------------------------------------------------------- */
static truing_status_t tmc_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out)
{
    truing_wheel_drive_tmc2209_ctx_t *c = (truing_wheel_drive_tmc2209_ctx_t *)self->ctx;
    if (!c->configured) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    if (!on) {
        halt_pulses(c);   /* immediate: no deceleration once the coils are off anyway */
        gpio_set_level((gpio_num_t)c->pins.en_gpio, 1);
        c->enabled = false;
    } else {
        if (!c->enabled) {
            /* A fresh enable clears what a restart can clear; a microstep mismatch is a
             * configuration fact and stays until the configuration changes. */
            c->fault_detail &= TRUING_TMC2209_FAULT_MICROSTEP_MISMATCH;
            c->faulted = c->fault_detail != 0u;
        }
        gpio_set_level((gpio_num_t)c->pins.en_gpio, 0);
        c->enabled = true;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

static truing_status_t tmc_move(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out)
{
    truing_wheel_drive_tmc2209_ctx_t *c = (truing_wheel_drive_tmc2209_ctx_t *)self->ctx;
    truing_reason_t reason = TRUING_REASON_NONE;
    if (!c->configured) {
        reason = TRUING_REASON_NOT_IMPLEMENTED;
    } else if (!c->enabled || c->faulted) {
        reason = TRUING_REASON_SENSOR_TIMEOUT;   /* disabled or faulted: motion refused */
    } else if (c->moving) {
        reason = TRUING_REASON_VALUE_OUT_OF_RANGE;   /* one move at a time */
    }
    if (reason == TRUING_REASON_NONE && steps != 0) {
        const uint32_t magnitude = steps < 0 ? (uint32_t)(-(int64_t)steps) : (uint32_t)steps;
        if (c->profile.tick_hz != TICK_HZ || !truing_ramp_plan(&c->ramp, &c->profile, magnitude)) {
            reason = TRUING_REASON_CALIBRATION_MISSING;   /* no usable motion profile */
        }
    }
    if (reason != TRUING_REASON_NONE) {
        c->refused++;
        if (reason_out != NULL) {
            *reason_out = reason;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    if (steps == 0) {
        return TRUING_STATUS_VALID;
    }
    c->dir_sign = steps > 0 ? 1 : -1;
    gpio_set_level((gpio_num_t)c->pins.dir_gpio, steps > 0 ? 1 : 0);
    esp_rom_delay_us(DIR_SETUP_US);
    c->moves++;
    c->moving = true;
    const gptimer_alarm_config_t alarm = {
        .alarm_count = (uint64_t)c->ramp.delay,
        .flags.auto_reload_on_alarm = false,
    };
    if (gptimer_set_raw_count(c->timer, 0) != ESP_OK || gptimer_set_alarm_action(c->timer, &alarm) != ESP_OK ||
        gptimer_start(c->timer) != ESP_OK) {
        c->moving = false;
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_SENSOR_TIMEOUT;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return TRUING_STATUS_VALID;
}

static void tmc_stop(truing_wheel_drive_if_t *self)
{
    truing_wheel_drive_tmc2209_ctx_t *c = (truing_wheel_drive_tmc2209_ctx_t *)self->ctx;
    if (c->moving) {
        truing_ramp_request_stop(&c->ramp);   /* the ISR decelerates and ends the train */
    }
}

static bool tmc_moving(const truing_wheel_drive_if_t *self)
{
    const truing_wheel_drive_tmc2209_ctx_t *c = (const truing_wheel_drive_tmc2209_ctx_t *)self->ctx;
    return c->moving;
}

static int32_t tmc_position(const truing_wheel_drive_if_t *self)
{
    const truing_wheel_drive_tmc2209_ctx_t *c = (const truing_wheel_drive_tmc2209_ctx_t *)self->ctx;
    return c->position;
}

static bool tmc_fault(const truing_wheel_drive_if_t *self, uint32_t *detail_out)
{
    truing_wheel_drive_tmc2209_ctx_t *c = (truing_wheel_drive_tmc2209_ctx_t *)self->ctx;
    /* DIAG is sampled only while enabled: what it does with the driver off is a bench
     * question (docs/ROLLER_BENCH.md R1), not something this driver assumes. */
    if (c->enabled && truing_wheel_drive_tmc2209_diag_level(c)) {
        c->fault_detail |= TRUING_TMC2209_FAULT_DIAG;
        c->faulted = true;
        halt_pulses(c);
    }
    if (detail_out != NULL) {
        *detail_out = c->fault_detail;
    }
    return c->faulted;
}

bool truing_wheel_drive_tmc2209_set_profile(truing_wheel_drive_tmc2209_ctx_t *ctx, const truing_ramp_profile_t *profile)
{
    if (ctx == NULL || profile == NULL || ctx->moving) {
        return false;
    }
    ctx->profile = *profile;
    return true;
}

bool truing_wheel_drive_tmc2209_zero(truing_wheel_drive_tmc2209_ctx_t *ctx)
{
    if (ctx == NULL || ctx->moving) {
        return false;
    }
    ctx->position = 0;
    return true;
}

static bool gpio_out(int gpio, int level)
{
    /* Level first, then direction: the pin never drives the opposite level, not even briefly. */
    gpio_set_level((gpio_num_t)gpio, (uint32_t)level);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io) == ESP_OK && gpio_set_level((gpio_num_t)gpio, (uint32_t)level) == ESP_OK;
}

static void probe_uart(truing_wheel_drive_tmc2209_ctx_t *c, unsigned microsteps)
{
    const uart_port_t port = (uart_port_t)c->pins.uart_num;
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(port, 256, 0, 0, NULL, 0) != ESP_OK || uart_param_config(port, &cfg) != ESP_OK ||
        uart_set_pin(port, c->pins.uart_tx_gpio, c->pins.uart_rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGW(TAG, "UART%d setup failed; driver configuration cannot be checked", c->pins.uart_num);
        c->pins.uart_num = -1;
        return;
    }
    uint32_t ioin = 0u;
    c->uart_ok = truing_wheel_drive_tmc2209_read(c, TRUING_TMC2209_IOIN, &ioin) &&
                 (ioin >> 24) == TRUING_TMC2209_VERSION;
    if (!c->uart_ok) {
        ESP_LOGW(TAG, "TMC2209 did not answer on UART%d node %u (IOIN %s)", c->pins.uart_num, c->pins.node,
                 ioin != 0u ? "wrong version" : "no reply");
    } else {
        truing_wheel_drive_tmc2209_read(c, TRUING_TMC2209_GCONF, &c->gconf);
        truing_wheel_drive_tmc2209_read(c, TRUING_TMC2209_CHOPCONF, &c->chopconf);
        ESP_LOGI(TAG, "TMC2209 on UART%d node %u: GCONF 0x%08lx CHOPCONF 0x%08lx (MRES %u usteps, %s)",
                 c->pins.uart_num, c->pins.node, (unsigned long)c->gconf, (unsigned long)c->chopconf,
                 truing_tmc2209_chopconf_microsteps(c->chopconf),
                 (c->gconf & TRUING_TMC2209_GCONF_MSTEP_REG_SELECT) ? "from MRES" : "MS1/MS2 pins govern");
    }
    if (microsteps == 0u) {
        return;
    }
    if (!c->uart_ok) {
        ESP_LOGW(TAG, "calibration expects %u microsteps; NOT verifiable without the UART", microsteps);
        return;
    }
    if (!truing_wheel_drive_tmc2209_set_microsteps(c, microsteps)) {
        c->fault_detail |= TRUING_TMC2209_FAULT_MICROSTEP_MISMATCH;
        c->faulted = true;
        ESP_LOGE(TAG, "could not set and verify %u microsteps; moves refused", microsteps);
    } else {
        ESP_LOGI(TAG, "microsteps set and verified: %u", microsteps);
    }
}

bool truing_wheel_drive_tmc2209_init(truing_wheel_drive_if_t *self, truing_wheel_drive_tmc2209_ctx_t *ctx,
                                     const truing_tmc2209_pins_t *pins, const truing_ramp_profile_t *profile,
                                     unsigned microsteps)
{
    if (self == NULL || ctx == NULL || pins == NULL) {
        return false;
    }
    memset(self, 0, sizeof(*self));
    memset(ctx, 0, sizeof(*ctx));
    ctx->pins = *pins;
    if (profile != NULL) {
        ctx->profile = *profile;
    }

    /* EN first, and disabled: nothing may energise the motor before enable(true). */
    bool ok = gpio_out(pins->en_gpio, 1) && gpio_out(pins->step_gpio, 0) && gpio_out(pins->dir_gpio, 0);
    if (ok && pins->diag_gpio >= 0) {
        const gpio_config_t diag = {
            .pin_bit_mask = 1ULL << pins->diag_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* unconnected must not read as a fault */
            .intr_type = GPIO_INTR_DISABLE,
        };
        ok = gpio_config(&diag) == ESP_OK;
    }
    if (ok) {
        const gptimer_config_t timer_cfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = TICK_HZ,
        };
        ok = gptimer_new_timer(&timer_cfg, &ctx->timer) == ESP_OK;
    }
    if (ok) {
        const gptimer_event_callbacks_t cbs = { .on_alarm = step_alarm_isr };
        ok = gptimer_register_event_callbacks(ctx->timer, &cbs, ctx) == ESP_OK && gptimer_enable(ctx->timer) == ESP_OK;
    }
    ctx->configured = ok;
    if (ok && pins->uart_num >= 0) {
        probe_uart(ctx, microsteps);
    }
    ESP_LOGI(TAG, "TMC2209 drive %s: STEP %d DIR %d EN %d DIAG %d, starts DISABLED", ok ? "ready" : "FAILED",
             pins->step_gpio, pins->dir_gpio, pins->en_gpio, pins->diag_gpio);

    self->impl_name = "wheel_drive_tmc2209";
    self->source_impl = TRUING_SOURCE_REAL;
    self->enable = tmc_enable;
    self->move_relative = tmc_move;
    self->stop = tmc_stop;
    self->is_moving = tmc_moving;
    self->position_steps = tmc_position;
    self->fault = tmc_fault;
    self->ctx = ctx;
    return ok;
}
