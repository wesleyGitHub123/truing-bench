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

/* Wheel Navigation subsystem (SPEC §10A) — owns these pins exclusively.
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

/* Excitation actuator output (reserved for the acoustic subsystem). */
#define BOARD_PLUCK_ACTUATOR_GPIO      15
/* 1 once a solenoid is physically wired to that pin. The DevKit has none; while 0 the pluck
 * seam is absent and the operator plucks at the station (see board_nano_esp32.h). */
#define BOARD_PLUCK_ACTUATOR_PRESENT   0

/* Status indication. The DevKitC-1 has only an addressable RGB LED (WS2812 on GPIO38). */
#define BOARD_HAS_PLAIN_STATUS_LED     0
#define BOARD_STATUS_LED_GPIO          38
#define BOARD_STATUS_LED_ACTIVE_LOW    0

#endif /* TRUING_BOARD_S3_DEVKIT_H */
