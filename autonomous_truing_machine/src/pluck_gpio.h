/* Excitation actuator on the target: one active-high pulse on BOARD_PLUCK_ACTUATOR_GPIO.
 * Whether a physical plucker is wired to that pin is a property of the machine build; this
 * driver reports that it commanded the pulse, nothing more. */
#ifndef TRUING_PLUCK_GPIO_H
#define TRUING_PLUCK_GPIO_H

#include <stdbool.h>

#include "truing_hal/pluck_if.h"

typedef struct {
    int      gpio;
    bool     configured;
    unsigned fires;
    unsigned last_pulse_us_measured;   /* esp_timer-measured duration of the last pulse */
} truing_pluck_gpio_ctx_t;

bool truing_pluck_gpio_init(truing_pluck_if_t *self, truing_pluck_gpio_ctx_t *ctx, int gpio);

#endif /* TRUING_PLUCK_GPIO_H */
