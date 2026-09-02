/**
 * @file board_nano_esp32.h
 * Arduino Nano ESP32 (u-blox NORA-W106: ESP32-S3, 16 MB flash, 8 MB PSRAM).
 *
 * Nano header label -> ESP32-S3 GPIO (from the Arduino pinout):
 *   D2=5  D3=6  D4=7  D5=8  D6=9  D7=10  D8=17  D9=18  D10=21  D11=38  D12=47  D13=48 (LED_BUILTIN)
 *   A0=1  A1=2  A2=3  A3=4  A4=11 A5=12  A6=13  A7=14   RX=44  TX=43
 *   RGB LED: red=46, green=0, blue=45 (active-low; all three are strapping pins)
 *
 * Native USB only: debug and UI transport share one link (SPEC §4.1, §12.1).
 */
#ifndef TRUING_BOARD_NANO_ESP32_H
#define TRUING_BOARD_NANO_ESP32_H

#define BOARD_NAME                     "arduino-nano-esp32"
#define BOARD_FLASH_EXPECTED_BYTES     (16u * 1024u * 1024u)
#define BOARD_PSRAM_EXPECTED_BYTES     (8u * 1024u * 1024u)
#define BOARD_HAS_SEPARATE_DEBUG_PORT  0

/* INMP441 on D5/D6/D7. */
#define BOARD_I2S_MIC_BCLK_GPIO        8
#define BOARD_I2S_MIC_WS_GPIO          9
#define BOARD_I2S_MIC_DIN_GPIO         10

/* Wheel Navigation subsystem (SPEC §10A). Index / reference sensor on D2. */
#define BOARD_INDEX_SENSOR_GPIO        5
/* Wheel-drive actuator, RESERVED for the Stage 2 development motor driver
 * (expected first: TMC2209). STEP=D8, DIR=D9, EN=D10, UART TX=D11, RX=D12, DIAG=A6.
 * Not validated; not a final hardware commitment. */
#define BOARD_WHEEL_DRIVE_STEP_GPIO    17
#define BOARD_WHEEL_DRIVE_DIR_GPIO     18
#define BOARD_WHEEL_DRIVE_ENABLE_GPIO  21
#define BOARD_WHEEL_DRIVE_UART_NUM     1
#define BOARD_WHEEL_DRIVE_UART_TX_GPIO 38
#define BOARD_WHEEL_DRIVE_UART_RX_GPIO 47
#define BOARD_WHEEL_DRIVE_DIAG_GPIO    13

/* Excitation actuator on D3. */
#define BOARD_PLUCK_ACTUATOR_GPIO      6

/* Yellow built-in LED on D13 (GPIO48), active-high. */
#define BOARD_HAS_PLAIN_STATUS_LED     1
#define BOARD_STATUS_LED_GPIO          48
#define BOARD_STATUS_LED_ACTIVE_LOW    0

#endif /* TRUING_BOARD_NANO_ESP32_H */
