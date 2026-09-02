#include "truing_hal/clock_if.h"

#include <stddef.h>

static uint32_t fake_now(void *ctx)
{
    const truing_fake_clock_t *fc = (const truing_fake_clock_t *)ctx;
    return fc != NULL ? fc->now_ms : 0u;
}

void truing_fake_clock_init(truing_fake_clock_t *fc, truing_clock_if_t *out, uint32_t start_ms)
{
    if (fc == NULL || out == NULL) {
        return;
    }
    fc->now_ms = start_ms;
    out->now_ms = fake_now;
    out->ctx = fc;
}

void truing_fake_clock_advance(truing_fake_clock_t *fc, uint32_t delta_ms)
{
    if (fc != NULL) {
        fc->now_ms += delta_ms;
    }
}
