#ifndef CONFIG_MOCK_MBEDTLS_X509_CRT_H
#define CONFIG_MOCK_MBEDTLS_X509_CRT_H

/*
 * Заглушка mbedtls/x509_crt.h для host-стенда конфигурации.
 *
 * В образе kouki::Config::set_ca_pem() проверяет PEM настоящим mbedtls
 * (mbedtls_x509_crt_parse). На хосте mbedtls нет, поэтому стенд подменяет
 * только API: разбор ограничен проверкой, что PEM-маркеры на месте и внутри
 * непустое base64-тело. Это значит, что host-проверка слабее образа — она
 * ловит «мусор вместо PEM», но не подделку ASN.1. В образе валидация полная.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int mock_parsed;
} mbedtls_x509_crt;

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt);
void mbedtls_x509_crt_free(mbedtls_x509_crt *crt);
int mbedtls_x509_crt_parse(mbedtls_x509_crt *chain, const unsigned char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif  // CONFIG_MOCK_MBEDTLS_X509_CRT_H
