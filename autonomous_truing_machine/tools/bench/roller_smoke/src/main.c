/* Roller drive bench, driven by hand from the serial console (tools/bench/roller_ctl.py).
 *
 * DevKit + TMC2209 + NEMA 17 + polyurethane roller, on the pins board_s3_devkit.h declares
 * (BOARD_WHEEL_DRIVE_*). Runs the REAL src/wheel_drive_tmc2209.c: STEP pulses from a gptimer ISR
 * paced by the tested trapezoidal ramp, every emitted pulse counted. The counter is the odometry
 * the calibration is built from (docs/ROLLER_BENCH.md), so it only ever changes by pulses that
 * actually went out -- 'z' zeroes it, nothing else touches it.
 *
 * NOTHING MOVES ON BOOT. The driver starts disabled (EN high). 'e' energises the coils, every
 * motion is a keystroke, and 'd' cuts the coils immediately (no deceleration) as the panic key.
 * Any key during a run or repeat test stops it (controlled deceleration).
 *
 * The rate and acceleration below are CAUTIOUS STARTING KNOBS for a first spin, not
 * characterized values: stage R2 finds what the motor and roller actually hold, and that is what
 * gets recorded. Direction: '+' means DIR high = positive actuator steps. Which way that turns
 * the wheel is a bench observation (R3), not something this sketch knows.
 *
 *   e / d      enable (EN low) / disable (EN high, immediate)
 *   f / b      jog + / - by the jog size
 *   F / B      run + / - until any key (controlled stop)
 *   x / space  controlled stop
 *   1..5       jog size 1 / 10 / 100 / 1000 / 10000 steps
 *   j          type an exact jog size, Enter to set (e.g. the steps for one spoke pitch)
 *   r / R      repeat test: `repeat count` jogs + / -, 1 s apart, position after each; any key aborts
 *   k          type the repeat count, Enter to set
 *   + / -      cruise rate x1.25 / /1.25
 *   ] / [      acceleration x1.25 / /1.25
 *   z          zero the step counter (only while stopped)
 *   p / s      position / full status
 *   u          TMC2209 register dump over UART
 *   a          scan UART node addresses 0..3
 *   w          raw UART probe: every byte that comes back after a read request, per node
 *   X          the 'w' check with TX and RX swapped in software (no rewiring)
 *   l          internal loopback: the UART and this code with no wire, plus the RX pin's idle level
 *   m          next microstep setting (written and verified over UART)
 *   i / I      IRUN -1 / +1 (writes IHOLD_IRUN; IHOLD = IRUN/2) -- changes motor current
 *   c          clear GSTAT
 *   ?          help
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOARD_PROFILE_S3_DEVKIT 1
#include "board/board_profile.h"
#include "truing_hal/tmc2209_frame.h"
#include "wheel_drive_tmc2209.h"

#define CONSOLE UART_NUM_0

static truing_wheel_drive_if_t g_drive;
static truing_wheel_drive_tmc2209_ctx_t g_ctx;
static truing_ramp_profile_t g_prof = { 1000000u, 800u, 1600u };   /* cautious first-spin knobs */
static int32_t g_jog = 100;
static unsigned g_repeat = 8u;
static unsigned g_irun = 31u;   /* IHOLD_IRUN is write-only: this is the part's reset value until written */
static bool g_irun_written = false;

static int key(TickType_t wait)
{
    uint8_t c;
    return uart_read_bytes(CONSOLE, &c, 1, wait) == 1 ? (int)c : -1;
}

/* Digits until Enter; any other key cancels. Returns false on cancel or no digits. */
static bool read_number(const char *what, uint32_t *out)
{
    printf("%s> ", what);
    fflush(stdout);
    uint32_t v = 0u;
    unsigned n = 0u;
    for (;;) {
        const int c = key(portMAX_DELAY);
        if (c >= '0' && c <= '9' && n < 9u) {
            v = v * 10u + (uint32_t)(c - '0');
            ++n;
            putchar(c);
            fflush(stdout);
        } else if (c == '\r' || c == '\n') {
            printf("\n");
            if (n == 0u) {
                return false;
            }
            *out = v;
            return true;
        } else {
            printf("  (cancelled)\n");
            return false;
        }
    }
}

static void print_status(void)
{
    uint32_t detail = 0u;
    const bool faulted = truing_wheel_drive_fault(&g_drive, &detail);
    printf("pos %ld steps | %s | %s | DIAG %d | fault %s (0x%lx)\n", (long)truing_wheel_drive_position_steps(&g_drive),
           g_ctx.enabled ? "ENABLED" : "disabled", g_ctx.moving ? "MOVING" : "stopped",
           truing_wheel_drive_tmc2209_diag_level(&g_ctx) ? 1 : 0, faulted ? "YES" : "no", (unsigned long)detail);
    printf("cruise %lu steps/s | accel %lu steps/s2 | jog %ld | repeat %u | est. %.2f s per jog\n",
           (unsigned long)g_prof.max_step_hz, (unsigned long)g_prof.accel_steps_s2, (long)g_jog, g_repeat,
           (double)truing_ramp_duration_s(&g_prof, (uint32_t)(g_jog < 0 ? -g_jog : g_jog)));
    printf("UART %s node %u | microsteps verified: %u | IRUN %u%s | moves %lu refused %lu\n",
           g_ctx.uart_ok ? "ok" : "NOT answering", g_ctx.pins.node, g_ctx.microsteps_verified, g_irun,
           g_irun_written ? "" : " (reset value, not written)", (unsigned long)g_ctx.moves,
           (unsigned long)g_ctx.refused);
}

static void reg_dump(void)
{
    static const struct {
        uint8_t     reg;
        const char *name;
    } k_regs[] = {
        { TRUING_TMC2209_IOIN, "IOIN" },         { TRUING_TMC2209_GCONF, "GCONF" },
        { TRUING_TMC2209_GSTAT, "GSTAT" },       { TRUING_TMC2209_IFCNT, "IFCNT" },
        { TRUING_TMC2209_CHOPCONF, "CHOPCONF" }, { TRUING_TMC2209_DRV_STATUS, "DRV_STATUS" },
        { TRUING_TMC2209_TSTEP, "TSTEP" },       { TRUING_TMC2209_SG_RESULT, "SG_RESULT" },
    };
    for (size_t i = 0; i < sizeof(k_regs) / sizeof(k_regs[0]); ++i) {
        uint32_t v = 0u;
        if (!truing_wheel_drive_tmc2209_read(&g_ctx, k_regs[i].reg, &v)) {
            printf("  %-10s  (no valid reply)\n", k_regs[i].name);
            continue;
        }
        printf("  %-10s  0x%08lx", k_regs[i].name, (unsigned long)v);
        if (k_regs[i].reg == TRUING_TMC2209_IOIN) {
            printf("  version 0x%02lx%s", (unsigned long)(v >> 24), (v >> 24) == TRUING_TMC2209_VERSION ? " (TMC2209)" : " (NOT a TMC2209?)");
        } else if (k_regs[i].reg == TRUING_TMC2209_GCONF) {
            printf("  i_scale_analog=%lu pdn_disable=%lu mstep_reg_select=%lu shaft=%lu spreadcycle=%lu",
                   (unsigned long)(v & 1u), (unsigned long)((v >> 6) & 1u), (unsigned long)((v >> 7) & 1u),
                   (unsigned long)((v >> 3) & 1u), (unsigned long)((v >> 2) & 1u));
        } else if (k_regs[i].reg == TRUING_TMC2209_GSTAT) {
            printf("  reset=%lu drv_err=%lu uv_cp=%lu", (unsigned long)(v & 1u), (unsigned long)((v >> 1) & 1u),
                   (unsigned long)((v >> 2) & 1u));
        } else if (k_regs[i].reg == TRUING_TMC2209_CHOPCONF) {
            printf("  MRES -> %u microsteps (applies only if mstep_reg_select=1)", truing_tmc2209_chopconf_microsteps(v));
        } else if (k_regs[i].reg == TRUING_TMC2209_DRV_STATUS) {
            char buf[64];
            truing_tmc2209_describe_drv_status(v, buf, sizeof(buf));
            printf("  %s  CS_ACTUAL %u/31", buf, truing_tmc2209_cs_actual(v));
        }
        printf("\n");
    }
}

static void scan_nodes(void)
{
    const uint8_t keep = g_ctx.pins.node;
    for (uint8_t n = 0u; n < 4u; ++n) {
        g_ctx.pins.node = n;
        uint32_t v = 0u;
        const bool ok = truing_wheel_drive_tmc2209_read(&g_ctx, TRUING_TMC2209_IOIN, &v);
        printf("  node %u: %s", n, ok ? "answers" : "silent");
        if (ok) {
            printf(", IOIN 0x%08lx", (unsigned long)v);
        }
        printf("\n");
    }
    g_ctx.pins.node = keep;
    printf("(node stays %u; the MS1/MS2 pins set it)\n", keep);
}

/* 'w': the UART wire itself, below the frame parser. Sends one IOIN read request per node and prints every
 * byte that comes back within 50 ms, so "no valid reply" can be told apart: nothing at all (RX not
 * connected / line not driven), only our own 4-byte echo (the line works, the driver is not answering:
 * power, VIO, address), or something else (garbled: level, baud or a fight on the line). */
/* One pass over nodes 0..3. Returns the best outcome: 0 nothing, 1 garbled, 2 echo only, 3 echo + reply. */
static int raw_pass(uart_port_t port)
{
    int best = 0;
    for (uint8_t n = 0u; n < 4u; ++n) {
        uint8_t req[TRUING_TMC2209_READ_REQ_LEN];
        truing_tmc2209_read_request(req, n, TRUING_TMC2209_IOIN);
        uart_flush_input(port);
        uart_write_bytes(port, req, sizeof(req));
        uart_wait_tx_done(port, pdMS_TO_TICKS(20));
        uint8_t buf[32];
        const int got = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(50));
        printf("    node %u sent %02x %02x %02x %02x | got %d byte%s:", n, req[0], req[1], req[2], req[3], got < 0 ? 0 : got,
               got == 1 ? "" : "s");
        for (int i = 0; i < got; ++i) {
            printf(" %02x", buf[i]);
        }
        const bool echo = got >= 4 && memcmp(buf, req, 4) == 0;
        const int r = got <= 0 ? 0 : (echo && got >= 12 ? 3 : (echo && got == 4 ? 2 : 1));
        static const char *const k_what[] = { "nothing", "unexpected bytes", "echo only", "echo + reply" };
        printf("  [%s]\n", k_what[r]);
        if (r > best) best = r;
    }
    return best;
}

/* The single-wire wiring check (TMC2209 datasheet, and what fixed the BigTreeTech V1.3 for others): the
 * board's TX reaches the driver's one UART pin THROUGH a resistor and its RX joins that same pin directly,
 * so an idle line reads high and every request comes back as an echo. The driver's logic runs from its own
 * regulator off VM, so VM must be powered; the probe runs once disabled and once with EN low. */
static void raw_probe(void)
{
    if (g_ctx.pins.uart_num < 0) {
        printf("UART not set up in this build\n");
        return;
    }
    const uart_port_t port = (uart_port_t)g_ctx.pins.uart_num;
    printf("UART%d TX GPIO %d RX GPIO %d, 115200 8N1\n", g_ctx.pins.uart_num, g_ctx.pins.uart_tx_gpio, g_ctx.pins.uart_rx_gpio);
    const int idle = gpio_get_level((gpio_num_t)g_ctx.pins.uart_rx_gpio);
    /* The UART driver pulls RX up internally, so high proves nothing; low means something holds the line down
     * (the TMC2209's PDN_UART pull-down, with TX not reaching it). Only the echo below is conclusive. */
    printf("  RX idle level %d %s\n", idle, idle ? "(high: inconclusive, RX has an internal pull-up)"
                                                : "(LOW: the driver's pin is on RX but TX is not reaching it)");
    printf("  pass 1, driver disabled (EN high):\n");
    int best = raw_pass(port);
    if (best < 3) {
        truing_wheel_drive_enable(&g_drive, true, NULL);
        vTaskDelay(pdMS_TO_TICKS(50));
        printf("  pass 2, driver ENABLED (EN low, coils energised, nothing moves):\n");
        const int b2 = raw_pass(port);
        truing_wheel_drive_enable(&g_drive, false, NULL);
        if (b2 > best) best = b2;
    }
    printf("  VERDICT: ");
    if (best == 3) {
        printf("the driver answers. UART works.\n");
    } else if (best == 2) {
        printf("the line works (echo) but the driver never answers: VM not powered, VIO/GND missing, or the joined\n"
               "           node is not the driver's UART pin. Try a 10k in place of the 1k (fixed the BTT V1.3 for others).\n");
    } else if (best == 1) {
        printf("bytes come back garbled: two outputs fighting on the line -- the TX resistor is missing or too small.\n");
    } else {
        printf("nothing comes back, not even the echo: GPIO %d and GPIO %d are not on one node. Wire\n"
               "           GPIO %d -> 1k (or 10k) -> driver UART pin, and GPIO %d -> that SAME pin, direct.\n",
               g_ctx.pins.uart_tx_gpio, g_ctx.pins.uart_rx_gpio, g_ctx.pins.uart_tx_gpio, g_ctx.pins.uart_rx_gpio);
    }
}

/* 'X': the same wire check with TX and RX swapped in software -- transmit on the RX GPIO, listen on the TX
 * GPIO -- for a module whose TX/RX pins are two separate directions wired straight-through instead of
 * crossed. Needs no rewiring; the board's pins are restored afterwards. */
static void swapped_probe(void)
{
    if (g_ctx.pins.uart_num < 0) {
        printf("UART not set up in this build\n");
        return;
    }
    const uart_port_t port = (uart_port_t)g_ctx.pins.uart_num;
    const int tx = g_ctx.pins.uart_tx_gpio, rx = g_ctx.pins.uart_rx_gpio;
    uart_set_pin(port, rx, tx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    printf("SWAPPED: transmitting on GPIO %d, listening on GPIO %d\n", rx, tx);
    printf("  pass 1, driver disabled (EN high):\n");
    int best = raw_pass(port);
    truing_wheel_drive_enable(&g_drive, true, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("  pass 2, driver ENABLED (EN low):\n");
    const int b2 = raw_pass(port);
    truing_wheel_drive_enable(&g_drive, false, NULL);
    if (b2 > best) best = b2;
    uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    printf("  restored: TX GPIO %d, RX GPIO %d\n", tx, rx);
    static const char *const k_what[] = { "nothing back", "garbled bytes", "echo only (line joined, driver silent)",
                                          "THE DRIVER ANSWERS with the pins swapped: swap them in board_s3_devkit.h" };
    printf("  VERDICT (swapped): %s\n", k_what[best]);
}

/* 'l': the UART peripheral and this code, with no wire involved -- the chip's internal loopback routes TX
 * straight to RX. Four bytes back means the driver, the port and the read path work, so a silent 'w' is
 * the wiring or the TMC2209; nothing back here means the software/peripheral setup itself is wrong. Also
 * reports the RX pin's level: an idle UART line sits high. */
static void loopback_probe(void)
{
    if (g_ctx.pins.uart_num < 0) {
        printf("UART not set up in this build\n");
        return;
    }
    const uart_port_t port = (uart_port_t)g_ctx.pins.uart_num;
    printf("RX GPIO %d level now: %d (an idle UART line reads 1)\n", g_ctx.pins.uart_rx_gpio,
           gpio_get_level((gpio_num_t)g_ctx.pins.uart_rx_gpio));
    uart_set_loop_back(port, true);
    uint8_t req[TRUING_TMC2209_READ_REQ_LEN];
    truing_tmc2209_read_request(req, 0u, TRUING_TMC2209_IOIN);
    uart_flush_input(port);
    uart_write_bytes(port, req, sizeof(req));
    uart_wait_tx_done(port, pdMS_TO_TICKS(20));
    uint8_t buf[16];
    const int got = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(50));
    uart_set_loop_back(port, false);
    printf("internal loopback: sent 4, got %d:", got < 0 ? 0 : got);
    for (int i = 0; i < got; ++i) {
        printf(" %02x", buf[i]);
    }
    printf("  [%s]\n", got == 4 && memcmp(buf, req, 4) == 0 ? "software + UART peripheral OK"
                                                            : "FAILED: the UART setup itself is wrong");
}

static void next_microsteps(void)
{
    static const unsigned k_order[] = { 8u, 16u, 32u, 64u, 128u, 256u, 1u, 2u, 4u };
    uint32_t chop = 0u;
    unsigned now = 0u;
    if (truing_wheel_drive_tmc2209_read(&g_ctx, TRUING_TMC2209_CHOPCONF, &chop)) {
        now = truing_tmc2209_chopconf_microsteps(chop);
    }
    unsigned next = 8u;
    for (size_t i = 0; i < sizeof(k_order) / sizeof(k_order[0]); ++i) {
        if (k_order[i] == now) {
            next = k_order[(i + 1u) % (sizeof(k_order) / sizeof(k_order[0]))];
        }
    }
    const bool ok = truing_wheel_drive_tmc2209_set_microsteps(&g_ctx, next);
    printf("microsteps %u -> %u: %s\n", now, next, ok ? "written and verified" : "FAILED (check UART wiring / node)");
    if (ok) {
        printf("NOTE: every step count measured so far was at %u microsteps; zero the counter ('z').\n", now);
    }
}

static void set_irun(unsigned irun)
{
    if (irun > 31u) {
        return;
    }
    const bool ok = truing_wheel_drive_tmc2209_write(&g_ctx, TRUING_TMC2209_IHOLD_IRUN,
                                                     truing_tmc2209_ihold_irun(irun / 2u, irun, 8u));
    if (ok) {
        g_irun = irun;
        g_irun_written = true;
    }
    printf("IRUN %u/31 of the VREF full scale (IHOLD %u): %s\n", irun, irun / 2u, ok ? "written" : "FAILED");
}

static void move(int32_t steps)
{
    truing_reason_t why = TRUING_REASON_NONE;
    const int64_t t0 = esp_timer_get_time();
    const int32_t p0 = truing_wheel_drive_position_steps(&g_drive);
    if (truing_wheel_drive_move_relative(&g_drive, steps, &why) != TRUING_STATUS_VALID) {
        printf("move %ld refused: %s\n", (long)steps,
               why == TRUING_REASON_SENSOR_TIMEOUT ? "driver disabled or faulted ('e' / 's')"
               : why == TRUING_REASON_VALUE_OUT_OF_RANGE ? "already moving"
                                                         : "bad profile");
        return;
    }
    bool stop_sent = false;
    while (truing_wheel_drive_is_moving(&g_drive)) {
        if (truing_wheel_drive_fault(&g_drive, NULL)) {
            printf("FAULT during move (DIAG high): pulses halted\n");
            break;
        }
        const int c = key(pdMS_TO_TICKS(10));
        if (c >= 0 && !stop_sent) {
            truing_wheel_drive_stop(&g_drive);
            stop_sent = true;
            if (c == 'd') {
                truing_wheel_drive_enable(&g_drive, false, NULL);
                printf("DISABLED mid-move (coils off, no deceleration)\n");
            }
        }
    }
    const int32_t p1 = truing_wheel_drive_position_steps(&g_drive);
    printf("%s %+ld steps in %.3f s -> pos %ld%s\n", stop_sent ? "stopped after" : "moved", (long)(p1 - p0),
           (double)(esp_timer_get_time() - t0) / 1e6, (long)p1, stop_sent ? " (cut short)" : "");
}

static void repeat_test(int32_t sign)
{
    printf("repeat: %u x %+ld steps, 1 s apart; any key aborts\n", g_repeat, (long)(sign * g_jog));
    for (unsigned i = 1u; i <= g_repeat; ++i) {
        move(sign * g_jog);
        printf("  [%u/%u] pos %ld\n", i, g_repeat, (long)truing_wheel_drive_position_steps(&g_drive));
        if (i < g_repeat && key(pdMS_TO_TICKS(1000)) >= 0) {
            printf("repeat aborted\n");
            return;
        }
    }
}

static void scale(uint32_t *v, bool up, uint32_t lo, uint32_t hi)
{
    uint32_t n = up ? (*v * 5u + 3u) / 4u : (*v * 4u) / 5u;
    if (n == *v) {
        n = up ? *v + 1u : *v - 1u;
    }
    *v = n < lo ? lo : (n > hi ? hi : n);
    truing_wheel_drive_tmc2209_set_profile(&g_ctx, &g_prof);
}

static void help(void)
{
    printf("e/d enable/disable | f/b jog +/- | F/B run +/- | x or space stop | 1..5 jog 1..10000 | j jog size\n"
           "r/R repeat +/- | k repeat count | +/- cruise | ]/[ accel | z zero | p pos | s status\n"
           "u registers | a scan nodes | m microsteps | i/I IRUN -/+ | c clear GSTAT | ? help\n");
}

void app_main(void)
{
    /* Console input: the DevKit's UART0 through its CH343 bridge (COM4). */
    uart_driver_install(CONSOLE, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONSOLE);
    esp_log_level_set("*", ESP_LOG_INFO);

    const truing_tmc2209_pins_t pins = {
        .step_gpio = BOARD_WHEEL_DRIVE_STEP_GPIO,
        .dir_gpio = BOARD_WHEEL_DRIVE_DIR_GPIO,
        .en_gpio = BOARD_WHEEL_DRIVE_ENABLE_GPIO,
        .diag_gpio = BOARD_WHEEL_DRIVE_DIAG_GPIO,
        .uart_num = BOARD_WHEEL_DRIVE_UART_NUM,
        .uart_tx_gpio = BOARD_WHEEL_DRIVE_UART_TX_GPIO,
        .uart_rx_gpio = BOARD_WHEEL_DRIVE_UART_RX_GPIO,
        .node = 0u,
    };
    /* microsteps 0: the bench reads the part as found and changes MRES only on 'm'. */
    const bool ok = truing_wheel_drive_tmc2209_init(&g_drive, &g_ctx, &pins, &g_prof, 0u);
    printf("\n=== roller_smoke: %s. Driver DISABLED; nothing moves until 'e'. ===\n",
           ok ? "drive ready" : "DRIVE INIT FAILED");
    printf("pins STEP %d DIR %d EN %d DIAG %d UART%d TX %d RX %d\n", pins.step_gpio, pins.dir_gpio, pins.en_gpio,
           pins.diag_gpio, pins.uart_num, pins.uart_tx_gpio, pins.uart_rx_gpio);
    help();
    print_status();

    for (;;) {
        const int c = key(portMAX_DELAY);
        uint32_t n = 0u;
        switch (c) {
        case 'e': {
            truing_reason_t why = TRUING_REASON_NONE;
            const bool en = truing_wheel_drive_enable(&g_drive, true, &why) == TRUING_STATUS_VALID;
            vTaskDelay(pdMS_TO_TICKS(20));
            printf("enable: %s | DIAG %d\n", en ? "EN low (coils energised)" : "FAILED",
                   truing_wheel_drive_tmc2209_diag_level(&g_ctx) ? 1 : 0);
            break;
        }
        case 'd':
            truing_wheel_drive_enable(&g_drive, false, NULL);
            printf("disabled: EN high, coils off | DIAG %d\n", truing_wheel_drive_tmc2209_diag_level(&g_ctx) ? 1 : 0);
            break;
        case 'f': move(g_jog); break;
        case 'b': move(-g_jog); break;
        case 'F': printf("running +; any key stops\n"); move(1000000000); break;
        case 'B': printf("running -; any key stops\n"); move(-1000000000); break;
        case 'x':
        case ' ': truing_wheel_drive_stop(&g_drive); printf("stop\n"); break;
        case '1': g_jog = 1; printf("jog 1\n"); break;
        case '2': g_jog = 10; printf("jog 10\n"); break;
        case '3': g_jog = 100; printf("jog 100\n"); break;
        case '4': g_jog = 1000; printf("jog 1000\n"); break;
        case '5': g_jog = 10000; printf("jog 10000\n"); break;
        case 'j':
            if (read_number("jog steps", &n) && n > 0u && n <= 100000000u) {
                g_jog = (int32_t)n;
                printf("jog %ld\n", (long)g_jog);
            }
            break;
        case 'k':
            if (read_number("repeat count", &n) && n > 0u && n <= 1000u) {
                g_repeat = (unsigned)n;
                printf("repeat %u\n", g_repeat);
            }
            break;
        case 'r': repeat_test(1); break;
        case 'R': repeat_test(-1); break;
        case '+': scale(&g_prof.max_step_hz, true, 10u, 200000u); printf("cruise %lu steps/s\n", (unsigned long)g_prof.max_step_hz); break;
        case '-': scale(&g_prof.max_step_hz, false, 10u, 200000u); printf("cruise %lu steps/s\n", (unsigned long)g_prof.max_step_hz); break;
        case ']': scale(&g_prof.accel_steps_s2, true, 10u, 2000000u); printf("accel %lu steps/s2\n", (unsigned long)g_prof.accel_steps_s2); break;
        case '[': scale(&g_prof.accel_steps_s2, false, 10u, 2000000u); printf("accel %lu steps/s2\n", (unsigned long)g_prof.accel_steps_s2); break;
        case 'z': printf(truing_wheel_drive_tmc2209_zero(&g_ctx) ? "counter zeroed\n" : "not while moving\n"); break;
        case 'p': printf("pos %ld\n", (long)truing_wheel_drive_position_steps(&g_drive)); break;
        case 's': print_status(); break;
        case 'u': reg_dump(); break;
        case 'a': scan_nodes(); break;
        case 'w': raw_probe(); break;
        case 'l': loopback_probe(); break;
        case 'X': swapped_probe(); break;
        case 'm': next_microsteps(); break;
        case 'i': if (g_irun > 0u) { set_irun(g_irun - 1u); } break;
        case 'I': set_irun(g_irun + 1u); break;
        case 'c':
            printf("GSTAT clear: %s\n", truing_wheel_drive_tmc2209_write(&g_ctx, TRUING_TMC2209_GSTAT, 0x7u) ? "written" : "FAILED");
            break;
        case '?': help(); break;
        case '\r':
        case '\n':
            break;
        default:
            printf("unknown key '%c' (? for help)\n", c >= 32 && c < 127 ? c : '?');
            break;
        }
    }
}
