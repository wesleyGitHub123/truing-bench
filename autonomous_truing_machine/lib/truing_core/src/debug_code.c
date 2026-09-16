#include "truing/debug_code.h"

static const char *const k_debug_code_str[TRUING_DEBUG_CODE__COUNT] = {
    "UNSET",
    "MEASURE_ONCE",
};

const char *truing_debug_code_str(truing_debug_code_t c)
{
    return (unsigned)c < TRUING_DEBUG_CODE__COUNT ? k_debug_code_str[c] : "?";
}
