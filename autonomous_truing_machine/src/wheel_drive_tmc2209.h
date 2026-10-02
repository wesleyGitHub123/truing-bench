/* Wheel drive on a TMC2209 in STEP/DIR mode, with its single-wire UART for identity and
 * configuration checks (SPEC §10A.6, §10A.8). A DEVELOPMENT driver behind wheel_drive_if.h,
 * not a committed final one.
 *
 * STEP pulses come from a gptimer alarm ISR paced by truing_ramp (lib/truing_hal), one pulse per
 * alarm, and the ISR counts every pulse it emits -- so position_steps() is exact even when a move
 * is cut short by stop() or by disabling the driver. RMT would time pulses with less CPU but
 * cannot say how many went out of an aborted transmission, which is the number navigation
 * needs. Flash writes (NVS) hold the ISR off while the cache is disabled: the pulse train
 * stretches, the count does not change.
 *
 * Wiring (board profile, BOARD_WHEEL_DRIVE_*): EN is active low and the driver is held
 * disabled from init until enable(true) -- an external pull-up on EN keeps it disabled through
 * reset as well. DIAG is read as the latched fault line; a pull-down keeps an unconnected DIAG
 * from reading as a fault. UART: TX through 10 kohm into PDN_UART (1 kohm did not answer on the BTT V1.3), RX straight to PDN_UART, so
 * every byte sent is also received and discarded.
 *
 * The driver never changes the motor CURRENT: that is set by the breakout's VREF (and IRUN,
 * left as the part powers up). It does, when the UART answers and a microstep count is given,
 * set MRES to that count and verify it -- the calibration was measured under one microstep
 * setting and is wrong under any other. Without the UART that cannot be checked, and the
 * driver says so rather than assuming. */
#ifndef TRUING_WHEEL_DRIVE_TMC2209_H
#define TRUING_WHEEL_DRIVE_TMC2209_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gptimer.h"

#include "truing_hal/step_ramp.h"
#include "truing_hal/wheel_drive_if.h"

typedef struct {
    int     step_gpio;
    int     dir_gpio;
    int     en_gpio;
    int     diag_gpio;       /* -1: not wired (faults undetectable) */
    int     uart_num;        /* -1: no UART */
    int     uart_tx_gpio;
    int     uart_rx_gpio;
    uint8_t node;            /* UART address from MS1/MS2, 0..3 */
} truing_tmc2209_pins_t;

/* fault() detail bits (for logs; not a reason code) */
#define TRUING_TMC2209_FAULT_DIAG               (1u << 0)
#define TRUING_TMC2209_FAULT_MICROSTEP_MISMATCH (1u << 1)
#define TRUING_TMC2209_FAULT_DRV_STATUS         (1u << 2)

typedef struct {
    truing_tmc2209_pins_t pins;
    truing_ramp_profile_t profile;
    gptimer_handle_t      timer;
    truing_ramp_t         ramp;
    volatile int32_t      position;
    volatile int32_t      dir_sign;
    volatile bool         moving;
    volatile bool         faulted;
    volatile uint32_t     fault_detail;
    bool                  enabled;
    bool                  configured;
    /* UART findings at init */
    bool                  uart_ok;               /* IOIN answered with VERSION 0x21 */
    uint32_t              gconf;
    uint32_t              chopconf;
    unsigned              microsteps_verified;   /* 0 = not verified (no UART, or none requested) */
    unsigned              microsteps_wanted;     /* what init was asked to enforce (0 = nothing to re-check) */
    bool                  mres_ok;               /* the last registers read back still held the wanted setting */
    uint32_t              mres_repairs;          /* times the setting had reverted and was rewritten */
    uint32_t              moves;
    uint32_t              refused;
} truing_wheel_drive_tmc2209_ctx_t;

/* `profile` paces every move (tick_hz must be 1 MHz, the timer's resolution); an invalid one
 * refuses moves. `microsteps` 0 leaves MRES alone; otherwise it is written and verified over the
 * UART, and a mismatch latches a fault. Returns false when the GPIO/timer setup failed, in which
 * case every call refuses. The driver starts DISABLED. */
bool truing_wheel_drive_tmc2209_init(truing_wheel_drive_if_t *self, truing_wheel_drive_tmc2209_ctx_t *ctx,
                                     const truing_tmc2209_pins_t *pins, const truing_ramp_profile_t *profile,
                                     unsigned microsteps);

/* Bench helpers (tools/bench/roller_smoke). Refused while a move is in progress. */
bool truing_wheel_drive_tmc2209_set_profile(truing_wheel_drive_tmc2209_ctx_t *ctx, const truing_ramp_profile_t *profile);
bool truing_wheel_drive_tmc2209_zero(truing_wheel_drive_tmc2209_ctx_t *ctx);
/* Raw register access over the single-wire UART (false: no UART, no reply, or a bad CRC). */
bool truing_wheel_drive_tmc2209_read(truing_wheel_drive_tmc2209_ctx_t *ctx, uint8_t reg, uint32_t *value);
bool truing_wheel_drive_tmc2209_write(truing_wheel_drive_tmc2209_ctx_t *ctx, uint8_t reg, uint32_t value);
/* Write MRES for `microsteps` (and GCONF.mstep_reg_select/pdn_disable), then read it back. */
bool truing_wheel_drive_tmc2209_set_microsteps(truing_wheel_drive_tmc2209_ctx_t *ctx, unsigned microsteps);
/* Re-read GCONF and CHOPCONF and compare with the setting init enforced. True when it still holds (or there is nothing to
 * check: no UART, no wanted count, or a move in progress -- the UART is not touched mid-move). When it does not hold:
 * with `repair` the setting is rewritten and verified (counted in mres_repairs) and the result is whether that worked;
 * without, false and nothing is written. A TMC2209 that browned out or reset comes back on its MS1/MS2 pins, so every
 * step count changes scale without any error being raised; this is how that is noticed. */
bool truing_wheel_drive_tmc2209_check_microsteps(truing_wheel_drive_tmc2209_ctx_t *ctx, bool repair);
bool truing_wheel_drive_tmc2209_diag_level(const truing_wheel_drive_tmc2209_ctx_t *ctx);

#endif /* TRUING_WHEEL_DRIVE_TMC2209_H */
