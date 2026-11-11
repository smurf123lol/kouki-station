/*
 * Заглушка mbedtls для стенда конфигурации: см. mbedtls/x509_crt.h.
 * Разбор намеренно грубый — маркеры PEM и непустое base64-тело.
 */
#include "mbedtls/x509_crt.h"

#include <stdbool.h>
#include <string.h>

#define MOCK_MBEDTLS_INVALID_ARG 0x0080

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt)
{
    if (crt != NULL) {
        crt->mock_parsed = 0;
    }
}

void mbedtls_x509_crt_free(mbedtls_x509_crt *crt)
{
    if (crt != NULL) {
        crt->mock_parsed = 0;
    }
}

int mbedtls_x509_crt_parse(mbedtls_x509_crt *chain, const unsigned char *buf, size_t buflen)
{
    if (chain == NULL || buf == NULL || buflen == 0) {
        return MOCK_MBEDTLS_INVALID_ARG;
    }

    const char *text = (const char *)buf;
    const char *begin = strstr(text, "-----BEGIN CERTIFICATE-----");
    const char *end = strstr(text, "-----END CERTIFICATE-----");
    if (begin == NULL || end == NULL || end <= begin) {
        return MOCK_MBEDTLS_INVALID_ARG;
    }

    /* Тело сертификата должно содержать хоть один символ base64. */
    const char *body = begin + strlen("-----BEGIN CERTIFICATE-----");
    const char *p = body;
    int base64_chars = 0;
    while (p < end) {
        const char c = *p;
        const bool is_base64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        if (is_base64) {
            base64_chars++;
        } else if (c != '\r' && c != '\n' && c != ' ' && c != '\t') {
            return MOCK_MBEDTLS_INVALID_ARG;
        }
        p++;
    }
    if (base64_chars < 16) {
        return MOCK_MBEDTLS_INVALID_ARG;
    }

    chain->mock_parsed = 1;
    return 0;
}
