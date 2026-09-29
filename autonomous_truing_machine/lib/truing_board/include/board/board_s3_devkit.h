/**
 * @file board_s3_devkit.h
 * ESP32-S3-DevKitC-1, N16R8 (ESP32-S3-WROOM-1-N16R8: 16 MB quad flash, 8 MB octal PSRAM).
 *
 * Pin selection constraints on this module:
 *   - GPIO26-32 carry the SPI flash; GPIO33-37 carry the octal PSRAM -> never used.
 *   - GPIO0, 3, 45, 46 are strapping pins -> avoided for peripherals.
 *   - GPIO19/20 are native USB D-/D+; GPIO43/44 are UART0 (CH343 bridge = debug console).
 *   - GPIO38 drives the on-board addressable RGB LED (v1.1 boards; v1.0 used GPIO48);
 *     it is not a plain LED.
 * The peripheral pins below are free ADC1-bank GPIOs on the left header.
 */
#ifndef TRUING_BOARD_S3_DEVKIT_H
#define TRUING_BOARD_S3_DEVKIT_H

#define BOARD_NAME                     "esp32-s3-devkitc-1-n16r8"
#define BOARD_FLASH_EXPECTED_BYTES     (16u * 1024u * 1024u)
#define BOARD_PSRAM_EXPECTED_BYTES     (8u * 1024u * 1024u)
#define BOARD_HAS_SEPARATE_DEBUG_PORT  1   /* UART bridge (debug) + native USB (UI transport), SPEC §12.1 */

/* INMP441 I2S microphone (SPEC §9.3). L/R pin of the module is tied for a fixed slot. */
#define BOARD_I2S_MIC_BCLK_GPIO        4
#define BOARD_I2S_MIC_WS_GPIO          5
#define BOARD_I2S_MIC_DIN_GPIO         6
/* Dual-mic bench wiring (TRUING_DUAL_MIC builds only): a second INMP441 shares BCLK/WS/DIN
 * above, its L/R pin strapped to 3V3 so it answers in the right slot (the acoustic front end
 * this workspace's Nano carried died to a wiring mistake 2026-09-27; both mics move to this
 * board). 0 = left slot (L/R to GND), 1 = right slot (L/R to 3V3). SD wants a pull-down to GND
 * at the board while both mics tri-state between slots. */
#define BOARD_I2S_MIC_INPUT_LEFT_STATION   0
#define BOARD_I2S_MIC_INPUT_RIGHT_STATION  1

/* Pin rule for a board carrying everything at once (s3_devkit_lab): the roller's pins below are FIXED by
 * its bench wiring (docs/ROLLER_BENCH.md R0) and everything else was placed around them -- the mics on
 * 4/5/6 and the solenoids on 15/21 share nothing with STEP/DIR/EN 16/17/18, UART 8/9 and DIAG 10.
 * Change a roller pin only with a bench entry; move the mics or solenoids instead.
 *
 * FROZEN FOR THE DEMO (user, 2026-09-29): this whole map -- mics 4/5/6, solenoids 15/21, roller
 * 16/17/18 + UART 8/9 + DIAG 10, index sensor 7 -- is expected to be the final deployed demo wiring.
 * Do not move a pin without telling the user. Still unproven on this board: the solenoid pins (B0
 * electrical check pending) and the roller UART (never answered on this driver).
 *
 * Wheel Navigation subsystem (SPEC §10A) — owns these pins exclusively.
 * Index / reference sensor input. */
#define BOARD_INDEX_SENSOR_GPIO        7
/* Wheel-drive actuator, RESERVED for the Stage 2 development motor driver (expected
 * first: TMC2209 in STEP/DIR mode with single-wire UART on TX/RX). Not validated;
 * not a final hardware commitment. */
#define BOARD_WHEEL_DRIVE_STEP_GPIO    16
#define BOARD_WHEEL_DRIVE_DIR_GPIO     17
#define BOARD_WHEEL_DRIVE_ENABLE_GPIO  18
#define BOARD_WHEEL_DRIVE_UART_NUM     1
#define BOARD_WHEEL_DRIVE_UART_TX_GPIO 8
#define BOARD_WHEEL_DRIVE_UART_RX_GPIO 9
#define BOARD_WHEEL_DRIVE_DIAG_GPIO    10

/* Excitation actuators, one per acoustic station -- moving here from the Nano (dead, see the
 * mic comment above). PRESENT stays 0 until each station is physically wired AND passes its own
 * B0 electrical check (tools/bench/solenoid_smoke's s3_bench_solenoid env), the same discipline
 * SOLENOID_CAMPAIGN.md applied on the Nano: gpio_config() succeeding is not evidence of a
 * solenoid, and a bare pin makes gpio_fire() return true for nothing (SPEC 17.3). */
#define BOARD_PLUCK_ACTUATOR_LEFT_GPIO     15
#define BOARD_PLUCK_ACTUATOR_LEFT_PRESENT  0
#define BOARD_PLUCK_ACTUATOR_RIGHT_GPIO    21
#define BOARD_PLUCK_ACTUATOR_RIGHT_PRESENT 0

/* Status indication. The DevKitC-1 has only an addressable RGB LED (WS2812 on GPIO38). */
#define BOARD_HAS_PLAIN_STATUS_LED     0
#define BOARD_STATUS_LED_GPIO          38
#define BOARD_STATUS_LED_ACTIVE_LOW    0

#endif /* TRUING_BOARD_S3_DEVKIT_H */
