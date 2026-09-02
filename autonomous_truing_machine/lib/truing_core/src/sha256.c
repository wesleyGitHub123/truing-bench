#include "truing/sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

static void transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16u; ++i) {
        w[i] = ((uint32_t)block[4u * i] << 24) | ((uint32_t)block[4u * i + 1u] << 16) |
               ((uint32_t)block[4u * i + 2u] << 8) | (uint32_t)block[4u * i + 3u];
    }
    for (unsigned i = 16u; i < 64u; ++i) {
        const uint32_t s0 = ROTR(w[i - 15u], 7) ^ ROTR(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        const uint32_t s1 = ROTR(w[i - 2u], 17) ^ ROTR(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64u; ++i) {
        const uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void truing_sha256_init(truing_sha256_t *ctx)
{
    static const uint32_t init[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(ctx->state, init, sizeof(init));
    ctx->bit_len = 0u;
    ctx->buffer_len = 0u;
}

void truing_sha256_update(truing_sha256_t *ctx, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return;
    }
    ctx->bit_len += (uint64_t)len * 8u;
    while (len > 0u) {
        const size_t take = (64u - ctx->buffer_len) < len ? (64u - ctx->buffer_len) : len;
        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len -= take;
        if (ctx->buffer_len == 64u) {
            transform(ctx->state, ctx->buffer);
            ctx->buffer_len = 0u;
        }
    }
}

void truing_sha256_final(truing_sha256_t *ctx, uint8_t digest[TRUING_SHA256_DIGEST_BYTES])
{
    const uint64_t bits = ctx->bit_len;
    const uint8_t pad = 0x80u;
    truing_sha256_update(ctx, &pad, 1u);
    ctx->bit_len -= 8u;   /* padding does not count toward the length */
    const uint8_t zero = 0u;
    while (ctx->buffer_len != 56u) {
        truing_sha256_update(ctx, &zero, 1u);
        ctx->bit_len -= 8u;
    }
    uint8_t len_be[8];
    for (unsigned i = 0; i < 8u; ++i) {
        len_be[i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    truing_sha256_update(ctx, len_be, 8u);
    for (unsigned i = 0; i < 8u; ++i) {
        digest[4u * i] = (uint8_t)(ctx->state[i] >> 24);
        digest[4u * i + 1u] = (uint8_t)(ctx->state[i] >> 16);
        digest[4u * i + 2u] = (uint8_t)(ctx->state[i] >> 8);
        digest[4u * i + 3u] = (uint8_t)(ctx->state[i]);
    }
}

void truing_sha256(const uint8_t *data, size_t len, uint8_t digest[TRUING_SHA256_DIGEST_BYTES])
{
    truing_sha256_t ctx;
    truing_sha256_init(&ctx);
    truing_sha256_update(&ctx, data, len);
    truing_sha256_final(&ctx, digest);
}
