#ifndef APP_HTTP_H
#define APP_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Создаёт esp_http_client_handle_t:
 * - Привязывает pinned CA (kouki_config_get_effective_ca_pem()) для https://
 * - Задаёт таймаут TLS из CONFIG_KOUKI_TLS_TIMEOUT_S
 * - Проверяет CONFIG_KOUKI_ALLOW_HTTP: если HTTP запрещён, отбивает http:// с кодом ESP_ERR_NOT_SUPPORTED
 * - Выставляет логгер успешной валидации сертификата на HTTP_EVENT_ON_CONNECTED
 */
esp_err_t kouki_http_new_client(const esp_http_client_config_t *base_cfg, esp_http_client_handle_t *out_client);

/**
 * Логирует успешное завершение рукопожатия TLS для соединений по https://.
 */
void kouki_http_on_connected(const char *url);

/**
 * Диагностирует причину ошибки соединения:
 * - Если сбой валидации сертификата (mbedtls / tls_flags):
 *     пишет в лог: tls: verify failed, err=0x<hex>
 *     пишет в err_buf: "Сертификат сервера не принят. Проверьте корневой CA."
 * - Если сбой DNS (getaddrinfo / CANNOT_RESOLVE_HOSTNAME):
 *     пишет в лог: dns: lookup failed for '<host>'
 *     пишет в err_buf: "Имя сервера не найдено: <host>."
 * - Если нет связи (TCP connect, timeout, etc.):
 *     пишет в err_buf: "Нет связи с сервером."
 */
void kouki_http_diagnose_error(esp_http_client_handle_t client, const char *url, esp_err_t req_err, char *err_buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // APP_HTTP_H
