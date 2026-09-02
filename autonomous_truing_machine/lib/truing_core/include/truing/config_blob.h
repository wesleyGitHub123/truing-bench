/**
 * @file config_blob.h
 * Versioned, integrity-checked serialization of configuration structures
 * (SPEC §11.5 persistence). Framework-free: the NVS/flash adapter stores the
 * bytes; this module defines and verifies their format.
 *
 * Wire format (all multi-byte fields little-endian):
 *
 *   offset  size  field
 *   0       4     magic          'T' 'R' 'C' 'F'
 *   4       2     schema_version TRUING_BLOB_SCHEMA_VERSION
 *   6       1     kind           truing_blob_kind_t
 *   7       1     reserved       0
 *   8       2     payload_len    bytes following the header
 *   10      4     crc32          CRC-32 (truing/crc32.h) over the payload bytes
 *   14      ...   payload        fields in the order documented per encoder
 *
 * Floats are IEEE-754 binary32 bit patterns. Booleans are one byte (0/1).
 * Enumerations are one byte. A decoder rejects: bad magic, unknown schema
 * version, wrong kind, CRC mismatch, truncated or over-long payload. It never
 * pads, truncates or reinterprets (SPEC P7).
 */
#ifndef TRUING_CONFIG_BLOB_H
#define TRUING_CONFIG_BLOB_H

#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_BLOB_MAGIC          0x46435254u   /* "TRCF" as little-endian u32 */
#define TRUING_BLOB_SCHEMA_VERSION 2u   /* 2: Phase 1f chain-profile DSP constants and tension-profile L_eff bounds */
#define TRUING_BLOB_HEADER_BYTES   14u
/* Upper bound on any encoded blob (largest payload is the wheel class, < 128 bytes). */
#define TRUING_BLOB_MAX_BYTES      256u

typedef enum {
    TRUING_BLOB_KIND_UNSET = 0,
    TRUING_BLOB_KIND_WHEEL_CLASS = 1,
    TRUING_BLOB_KIND_SOLVER = 2,
    TRUING_BLOB_KIND_CHAIN_PROFILE = 3,
    TRUING_BLOB_KIND_TENSION_MODEL_PROFILE = 4,
    TRUING_BLOB_KIND_MACHINE_PROFILE = 5,
    TRUING_BLOB_KIND__COUNT
} truing_blob_kind_t;

typedef enum {
    TRUING_BLOB_OK = 0,
    TRUING_BLOB_ERR_NULL,
    TRUING_BLOB_ERR_CAPACITY,        /* encode: output buffer too small */
    TRUING_BLOB_ERR_TRUNCATED,
    TRUING_BLOB_ERR_MAGIC,
    TRUING_BLOB_ERR_VERSION,
    TRUING_BLOB_ERR_KIND,
    TRUING_BLOB_ERR_CRC,
    TRUING_BLOB_ERR_PAYLOAD_LENGTH,  /* payload length disagrees with the kind's schema */
    TRUING_BLOB__COUNT
} truing_blob_result_t;

/* Encoders return the total blob size in bytes, or 0 on error (NULL or capacity). */
size_t truing_blob_encode_wheel_class(const truing_wheel_class_config_t *cfg, uint8_t *buf, size_t cap);
size_t truing_blob_encode_solver(const truing_solver_config_t *cfg, uint8_t *buf, size_t cap);
size_t truing_blob_encode_chain_profile(const truing_chain_profile_t *p, uint8_t *buf, size_t cap);
size_t truing_blob_encode_tension_model_profile(const truing_tension_model_profile_t *p, uint8_t *buf, size_t cap);
size_t truing_blob_encode_machine_profile(const truing_machine_profile_t *p, uint8_t *buf, size_t cap);

/* Decoders write `out` only on success. */
truing_blob_result_t truing_blob_decode_wheel_class(const uint8_t *buf, size_t len, truing_wheel_class_config_t *out);
truing_blob_result_t truing_blob_decode_solver(const uint8_t *buf, size_t len, truing_solver_config_t *out);
truing_blob_result_t truing_blob_decode_chain_profile(const uint8_t *buf, size_t len, truing_chain_profile_t *out);
truing_blob_result_t truing_blob_decode_tension_model_profile(const uint8_t *buf, size_t len, truing_tension_model_profile_t *out);
truing_blob_result_t truing_blob_decode_machine_profile(const uint8_t *buf, size_t len, truing_machine_profile_t *out);

/* Header inspection + integrity check without decoding the payload. */
truing_blob_result_t truing_blob_peek(const uint8_t *buf, size_t len, truing_blob_kind_t *kind_out,
                                      uint16_t *payload_len_out, uint32_t *crc_out);

const char *truing_blob_result_str(truing_blob_result_t r);
const char *truing_blob_kind_str(truing_blob_kind_t k);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_CONFIG_BLOB_H */
