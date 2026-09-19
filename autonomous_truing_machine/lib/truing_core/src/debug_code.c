#include "truing/debug_code.h"

#include "truing/config.h"   /* TRUING_EXCITATION_PULSE_MAX_MS: the same bound the pulse driver enforces */

static const char *const k_debug_code_str[TRUING_DEBUG_CODE__COUNT] = {
    "UNSET",
    "MEASURE_ONCE",
};

const char *truing_debug_code_str(truing_debug_code_t c)
{
    return (unsigned)c < TRUING_DEBUG_CODE__COUNT ? k_debug_code_str[c] : "?";
}

#define MEASURE_ONCE_NO_FIRE_BIT   (1u << 8)
#define MEASURE_ONCE_RESERVED_MASK (0x7Fu << 9)
#define MEASURE_ONCE_PULSE_SHIFT   16u

int32_t truing_measure_once_pack(const truing_measure_once_args_t *a)
{
    uint32_t v = (uint32_t)a->spoke_id;
    if (a->no_fire) {
        v |= MEASURE_ONCE_NO_FIRE_BIT;
    }
    v |= (uint32_t)a->pulse_ms << MEASURE_ONCE_PULSE_SHIFT;
    return (int32_t)v;
}

bool truing_measure_once_unpack(int32_t arg, truing_measure_once_args_t *out)
{
    if (arg < 0 || out == NULL) {
        return false;
    }
    const uint32_t v = (uint32_t)arg;
    if ((v & MEASURE_ONCE_RESERVED_MASK) != 0u) {
        return false;
    }
    const bool no_fire = (v & MEASURE_ONCE_NO_FIRE_BIT) != 0u;
    const uint16_t pulse_ms = (uint16_t)(v >> MEASURE_ONCE_PULSE_SHIFT);
    if (no_fire && pulse_ms != 0u) {
        return false;   /* a pulse width for a strike that is not going to happen */
    }
    if ((float)pulse_ms > TRUING_EXCITATION_PULSE_MAX_MS) {
        return false;
    }
    out->spoke_id = (uint8_t)(v & 0xFFu);
    out->no_fire = no_fire;
    out->pulse_ms = pulse_ms;
    return true;
}
