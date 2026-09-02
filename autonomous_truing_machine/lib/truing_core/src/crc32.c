#include "truing/crc32.h"

uint32_t truing_crc32_update(uint32_t state, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return state;
    }
    for (size_t i = 0; i < len; ++i) {
        state ^= data[i];
        for (unsigned b = 0; b < 8u; ++b) {
            state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1u)));
        }
    }
    return state;
}

uint32_t truing_crc32_final(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

uint32_t truing_crc32(const uint8_t *data, size_t len)
{
    return truing_crc32_final(truing_crc32_update(0xFFFFFFFFu, data, len));
}
