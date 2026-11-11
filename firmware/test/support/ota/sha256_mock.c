/*
 * SHA-256 для host-стенда Ota (T121 П8), см. mbedtls/sha256.h.
 *
 * Настоящий алгоритм (FIPS 180-4), а не заглушка: стенд проверяет ветки
 * «sha256 совпал / не совпал» реальными значениями, а не подставленным
 * результатом. Компилируется как C.
 */
#include "mbedtls/sha256.h"

#include <string.h>

static const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(mbedtls_sha256_context *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16)
               | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void mbedtls_sha256_init(mbedtls_sha256_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void mbedtls_sha256_free(mbedtls_sha256_context *ctx)
{
    /* Как в mbedtls с MBEDTLS_PLATFORM_ZEROIZE_ALT: контекст обнуляется. */
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

void mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{
    (void)is224; /* Стенду нужен только SHA-256. */
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buffer_len = 0;
}

void mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen)
{
    for (size_t i = 0; i < ilen; i++) {
        ctx->buffer[ctx->buffer_len++] = input[i];
        if (ctx->buffer_len == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->bitlen += 512;
            ctx->buffer_len = 0;
        }
    }
}

void mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32])
{
    size_t i = ctx->buffer_len;

    /* Padding: 0x80, нули, затем длина сообщения в битах (big-endian). */
    if (i < 56) {
        ctx->buffer[i++] = 0x80;
        while (i < 56) {
            ctx->buffer[i++] = 0x00;
        }
    } else {
        ctx->buffer[i++] = 0x80;
        while (i < 64) {
            ctx->buffer[i++] = 0x00;
        }
        sha256_transform(ctx, ctx->buffer);
        memset(ctx->buffer, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->buffer_len * 8u;
    for (int j = 0; j < 8; j++) {
        ctx->buffer[63 - j] = (uint8_t)(ctx->bitlen >> (j * 8));
    }
    sha256_transform(ctx, ctx->buffer);

    for (int j = 0; j < 8; j++) {
        output[j * 4] = (unsigned char)(ctx->state[j] >> 24);
        output[j * 4 + 1] = (unsigned char)(ctx->state[j] >> 16);
        output[j * 4 + 2] = (unsigned char)(ctx->state[j] >> 8);
        output[j * 4 + 3] = (unsigned char)(ctx->state[j]);
    }
}
