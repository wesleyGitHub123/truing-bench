/* Excitation actuator on the target: one active-high pulse on BOARD_PLUCK_ACTUATOR_GPIO.
 * Whether a physical plucker is wired to that pin is a property of the machine build; this
 * driver reports that it commanded the pulse, nothing more.
 *
 * The pulse's falling edge is set by a dedicated hardware timer's ISR (gptimer, one-shot), not
 * by a task waking up from vTaskDelay -- see pluck_gpio.c for why that distinction is the whole
 * point of this file. fire() still blocks until the pulse is confirmed complete. */
#ifndef TRUING_PLUCK_GPIO_H
#define TRUING_PLUCK_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "truing_hal/pluck_if.h"

typedef struct {
    int              gpio;
    bool             configured;
    unsigned         fires;
    unsigned         last_pulse_us_measured;   /* hardware-timed duration of the last pulse */
    gptimer_handle_t timer;
    SemaphoreHandle_t done_sem;
    int64_t          t_on_us;                  /* stashed by fire(), read back by the alarm ISR */
} truing_pluck_gpio_ctx_t;

bool truing_pluck_gpio_init(truing_pluck_if_t *self, truing_pluck_gpio_ctx_t *ctx, int gpio);

#endif /* TRUING_PLUCK_GPIO_H */
