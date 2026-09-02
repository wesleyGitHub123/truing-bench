/**
 * @file crc32.h
 * CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, init/xorout 0xFFFFFFFF).
 * Used for configuration-blob integrity (truing/config_blob.h).
 */
#ifndef TRUING_CRC32_H
#define TRUING_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t truing_crc32(const uint8_t *data, size_t len);
/* Incremental form: start with 0xFFFFFFFF, feed chunks, finish with truing_crc32_final(). */
uint32_t truing_crc32_update(uint32_t state, const uint8_t *data, size_t len);
uint32_t truing_crc32_final(uint32_t state);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_CRC32_H */
