/* TMC2209 UART datagrams and register fields (see tmc2209_frame.h). */
#include <stdio.h>
#include <string.h>

#include "truing_hal/tmc2209_frame.h"

uint8_t truing_tmc2209_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;
    for (size_t i = 0; data != NULL && i < len; ++i) {
        uint8_t b = data[i];
        for (unsigned j = 0; j < 8u; ++j) {
            if (((crc >> 7) ^ (b & 0x01u)) != 0u) {
                crc = (uint8_t)((crc << 1) ^ 0x07u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
            b = (uint8_t)(b >> 1);
        }
    }
    return crc;
}

void truing_tmc2209_write_frame(uint8_t out[TRUING_TMC2209_WRITE_LEN], uint8_t node, uint8_t reg, uint32_t value)
{
    out[0] = TRUING_TMC2209_SYNC;
    out[1] = node;
    out[2] = (uint8_t)(reg | TRUING_TMC2209_WRITE_BIT);
    out[3] = (uint8_t)(value >> 24);
    out[4] = (uint8_t)(value >> 16);
    out[5] = (uint8_t)(value >> 8);
    out[6] = (uint8_t)value;
    out[7] = truing_tmc2209_crc(out, 7u);
}

void truing_tmc2209_read_request(uint8_t out[TRUING_TMC2209_READ_REQ_LEN], uint8_t node, uint8_t reg)
{
    out[0] = TRUING_TMC2209_SYNC;
    out[1] = node;
    out[2] = (uint8_t)(reg & 0x7Fu);
    out[3] = truing_tmc2209_crc(out, 3u);
}

bool truing_tmc2209_parse_reply(const uint8_t in[TRUING_TMC2209_REPLY_LEN], uint8_t reg, uint32_t *value_out)
{
    if (in == NULL || in[0] != TRUING_TMC2209_SYNC || in[1] != TRUING_TMC2209_MASTER_ADDR || in[2] != (reg & 0x7Fu) ||
        in[7] != truing_tmc2209_crc(in, 7u)) {
        return false;
    }
    if (value_out != NULL) {
        *value_out = ((uint32_t)in[3] << 24) | ((uint32_t)in[4] << 16) | ((uint32_t)in[5] << 8) | (uint32_t)in[6];
    }
    return true;
}

#define MRES_SHIFT 24u
#define MRES_MASK  (0xFu << MRES_SHIFT)

unsigned truing_tmc2209_chopconf_microsteps(uint32_t chopconf)
{
    const unsigned mres = (unsigned)((chopconf & MRES_MASK) >> MRES_SHIFT);
    return mres <= 8u ? (256u >> mres) : 0u;
}

bool truing_tmc2209_chopconf_set_microsteps(uint32_t chopconf, unsigned microsteps, uint32_t *out)
{
    for (unsigned mres = 0; mres <= 8u; ++mres) {
        if ((256u >> mres) == microsteps) {
            if (out != NULL) {
                *out = (chopconf & ~MRES_MASK) | ((uint32_t)mres << MRES_SHIFT);
            }
            return true;
        }
    }
    return false;
}

uint32_t truing_tmc2209_ihold_irun(unsigned ihold, unsigned irun, unsigned iholddelay)
{
    return ((uint32_t)(ihold & 0x1Fu)) | ((uint32_t)(irun & 0x1Fu) << 8) | ((uint32_t)(iholddelay & 0xFu) << 16);
}

unsigned truing_tmc2209_cs_actual(uint32_t drv_status)
{
    return (unsigned)((drv_status >> 16) & 0x1Fu);
}

void truing_tmc2209_describe_drv_status(uint32_t v, char *buf, size_t cap)
{
    static const struct {
        uint32_t    bit;
        const char *name;
    } k_flags[] = {
        { TRUING_TMC2209_DRV_OT, "OT" },       { TRUING_TMC2209_DRV_OTPW, "OTPW" },
        { TRUING_TMC2209_DRV_S2GA, "S2GA" },   { TRUING_TMC2209_DRV_S2GB, "S2GB" },
        { TRUING_TMC2209_DRV_S2VSA, "S2VSA" }, { TRUING_TMC2209_DRV_S2VSB, "S2VSB" },
        { TRUING_TMC2209_DRV_OLA, "OLA" },     { TRUING_TMC2209_DRV_OLB, "OLB" },
        { TRUING_TMC2209_DRV_STST, "STST" },
    };
    if (buf == NULL || cap == 0u) {
        return;
    }
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < sizeof(k_flags) / sizeof(k_flags[0]); ++i) {
        if ((v & k_flags[i].bit) != 0u) {
            const int n = snprintf(buf + used, cap - used, "%s%s", used > 0u ? " " : "", k_flags[i].name);
            if (n < 0 || (size_t)n >= cap - used) {
                return;
            }
            used += (size_t)n;
        }
    }
    if (used == 0u) {
        snprintf(buf, cap, "ok");
    }
}
