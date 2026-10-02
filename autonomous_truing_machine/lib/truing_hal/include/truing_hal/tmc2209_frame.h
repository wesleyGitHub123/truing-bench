/**
 * @file tmc2209_frame.h
 * TMC2209 single-wire UART datagrams and register fields: bytes in, bytes out, no I/O.
 * Pure so the framing, CRC and field decoding are host-testable; the target supplies only
 * the UART (and discards the echo of its own transmission, since TX and RX share one wire).
 *
 * The TMC2209 is the expected first DEVELOPMENT driver behind the wheel-drive boundary
 * (SPEC §10A.6), not a committed final driver. Field layouts are the TMC2209 datasheet's.
 */
#ifndef TRUING_HAL_TMC2209_FRAME_H
#define TRUING_HAL_TMC2209_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_TMC2209_SYNC          0x05u
#define TRUING_TMC2209_MASTER_ADDR   0xFFu   /* address field of every reply */
#define TRUING_TMC2209_WRITE_BIT     0x80u
#define TRUING_TMC2209_WRITE_LEN     8u
#define TRUING_TMC2209_READ_REQ_LEN  4u
#define TRUING_TMC2209_REPLY_LEN     8u
#define TRUING_TMC2209_VERSION       0x21u   /* IOIN[31:24] of a TMC2209 */

/* Registers used by the bench and the development driver. */
#define TRUING_TMC2209_GCONF       0x00u
#define TRUING_TMC2209_GSTAT       0x01u
#define TRUING_TMC2209_IFCNT       0x02u
#define TRUING_TMC2209_IOIN        0x06u
#define TRUING_TMC2209_IHOLD_IRUN  0x10u   /* write-only */
#define TRUING_TMC2209_TSTEP       0x12u
#define TRUING_TMC2209_SG_RESULT   0x41u
#define TRUING_TMC2209_CHOPCONF    0x6Cu
#define TRUING_TMC2209_DRV_STATUS  0x6Fu

/* GCONF bits */
#define TRUING_TMC2209_GCONF_I_SCALE_ANALOG   (1u << 0)
#define TRUING_TMC2209_GCONF_EN_SPREADCYCLE   (1u << 2)
#define TRUING_TMC2209_GCONF_SHAFT            (1u << 3)
#define TRUING_TMC2209_GCONF_PDN_DISABLE      (1u << 6)
#define TRUING_TMC2209_GCONF_MSTEP_REG_SELECT (1u << 7)

/* DRV_STATUS bits */
#define TRUING_TMC2209_DRV_OTPW   (1u << 0)
#define TRUING_TMC2209_DRV_OT     (1u << 1)
#define TRUING_TMC2209_DRV_S2GA   (1u << 2)
#define TRUING_TMC2209_DRV_S2GB   (1u << 3)
#define TRUING_TMC2209_DRV_S2VSA  (1u << 4)
#define TRUING_TMC2209_DRV_S2VSB  (1u << 5)
#define TRUING_TMC2209_DRV_OLA    (1u << 6)
#define TRUING_TMC2209_DRV_OLB    (1u << 7)
#define TRUING_TMC2209_DRV_STST   (1u << 31)
/* Conditions that mean the driver cannot be trusted to have moved the motor as commanded:
 * over-temperature and short to ground / supply. Open load (OLA/OLB) is deliberately not in
 * the set: the datasheet notes it can flag at standstill or low speed without a fault. */
#define TRUING_TMC2209_DRV_FAULT_MASK \
    (TRUING_TMC2209_DRV_OT | TRUING_TMC2209_DRV_S2GA | TRUING_TMC2209_DRV_S2GB | \
     TRUING_TMC2209_DRV_S2VSA | TRUING_TMC2209_DRV_S2VSB)

/* CRC8 of `len` bytes, TMC bit order (polynomial x^8 + x^2 + x + 1, LSB of each byte first). */
uint8_t truing_tmc2209_crc(const uint8_t *data, size_t len);

/* Build a write datagram (8 bytes) / read request (4 bytes). `node` is 0..3 (MS1/MS2 pins). */
void truing_tmc2209_write_frame(uint8_t out[TRUING_TMC2209_WRITE_LEN], uint8_t node, uint8_t reg, uint32_t value);
void truing_tmc2209_read_request(uint8_t out[TRUING_TMC2209_READ_REQ_LEN], uint8_t node, uint8_t reg);

/* Validate a reply to a read of `reg` (sync, master address, register, CRC). */
bool truing_tmc2209_parse_reply(const uint8_t in[TRUING_TMC2209_REPLY_LEN], uint8_t reg, uint32_t *value_out);

/* Microsteps per full step encoded in CHOPCONF.MRES (256 >> MRES); 0 for a reserved value. */
unsigned truing_tmc2209_chopconf_microsteps(uint32_t chopconf);
/* CHOPCONF with MRES replaced for `microsteps` (a power of two, 1..256); false otherwise. */
bool truing_tmc2209_chopconf_set_microsteps(uint32_t chopconf, unsigned microsteps, uint32_t *out);
/* True when the registers still hold the microstep setting the host wrote: GCONF.mstep_reg_select set (MRES, not the
 * MS1/MS2 pins, governs) and CHOPCONF.MRES decoding to `want`. A driver that lost power or reset powers up on the pins
 * (8 microsteps on this bench: its MS1/MS2 are strapped to GND), so every step count silently halves. */
bool truing_tmc2209_microsteps_intact(uint32_t gconf, uint32_t chopconf, unsigned want);
/* IHOLD_IRUN value; each current is a 0..31 fraction ((n+1)/32) of the VREF full scale. */
uint32_t truing_tmc2209_ihold_irun(unsigned ihold, unsigned irun, unsigned iholddelay);
unsigned truing_tmc2209_cs_actual(uint32_t drv_status);

/* Human-readable DRV_STATUS flags into `buf` ("ok" when none are set). */
void truing_tmc2209_describe_drv_status(uint32_t drv_status, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_TMC2209_FRAME_H */
