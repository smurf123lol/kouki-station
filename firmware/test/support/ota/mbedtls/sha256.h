#pragma once

/*
 * Заглушка mbedtls/sha256.h для host-стенда Ota (T121 П8).
 *
 * sha256 считается настоящим (mbedtls не тянем): контекст — обычный
 * SHA-256, реализованный здесь же. Тесту это нужно, чтобы проверить ветки
 * «sha256 совпал / не совпал» настоящими значениями, а не подделкой.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buffer_len;
} mbedtls_sha256_context;

void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
void mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
void mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen);
void mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32]);

#ifdef __cplusplus
}
#endif
