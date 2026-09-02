/**
 * @file sha256.h
 * SHA-256 (FIPS 180-4) for artifact integrity checks (SPEC 8.7 content_hash). The
 * host computes the same digest with hashlib; the firmware must reproduce it, so
 * it lives in the framework-free core rather than behind a platform library.
 */
#ifndef TRUING_SHA256_H
#define TRUING_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_SHA256_DIGEST_BYTES 32u

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  buffer[64];
    size_t   buffer_len;
} truing_sha256_t;

void truing_sha256_init(truing_sha256_t *ctx);
void truing_sha256_update(truing_sha256_t *ctx, const uint8_t *data, size_t len);
void truing_sha256_final(truing_sha256_t *ctx, uint8_t digest[TRUING_SHA256_DIGEST_BYTES]);
void truing_sha256(const uint8_t *data, size_t len, uint8_t digest[TRUING_SHA256_DIGEST_BYTES]);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_SHA256_H */
