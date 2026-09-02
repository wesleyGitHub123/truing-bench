/**
 * @file board_profile.h
 * Selects the board profile from the build flag (SPEC §4.2 / §4.3):
 *
 *   -DBOARD_PROFILE_S3_DEVKIT   ESP32-S3-DevKitC-1 N16R8 (development target)
 *   -DBOARD_PROFILE_NANO        Arduino Nano ESP32 (final form-factor target)
 *
 * Every profile defines the same macro set so drivers never branch on board:
 *
 *   BOARD_NAME                         string
 *   BOARD_FLASH_EXPECTED_BYTES         verified at bring-up
 *   BOARD_PSRAM_EXPECTED_BYTES         verified at bring-up (SPEC §4.1: verify on the board)
 *   BOARD_HAS_SEPARATE_DEBUG_PORT      1 if debug and UI transport can use different physical ports (SPEC §12.1)
 *   BOARD_I2S_MIC_BCLK_GPIO / _WS_GPIO / _DIN_GPIO   INMP441 front end (SPEC §9.3)
 *   BOARD_INDEX_SENSOR_GPIO            wheel index / reference sensor (Wheel Navigation, SPEC §10A)
 *   BOARD_WHEEL_DRIVE_*                wheel-drive actuator pins, reserved for the C3 development
 *                                      motor driver (Wheel Navigation owns them; SPEC §10A)
 *   BOARD_PLUCK_ACTUATOR_GPIO          excitation actuator (reserved; acoustic subsystem owns it)
 *   BOARD_STATUS_LED_GPIO / BOARD_STATUS_LED_ACTIVE_LOW / BOARD_HAS_PLAIN_STATUS_LED
 */
#ifndef TRUING_BOARD_PROFILE_H
#define TRUING_BOARD_PROFILE_H

#if defined(BOARD_PROFILE_S3_DEVKIT) && defined(BOARD_PROFILE_NANO)
#error "Exactly one BOARD_PROFILE_* flag must be defined (both are set)."
#elif defined(BOARD_PROFILE_S3_DEVKIT)
#include "board/board_s3_devkit.h"
#elif defined(BOARD_PROFILE_NANO)
#include "board/board_nano_esp32.h"
#else
#error "No board profile selected: define BOARD_PROFILE_S3_DEVKIT or BOARD_PROFILE_NANO (see platformio.ini)."
#endif

#ifdef __cplusplus
extern "C" {
#endif

const char *truing_board_name(void);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_BOARD_PROFILE_H */
