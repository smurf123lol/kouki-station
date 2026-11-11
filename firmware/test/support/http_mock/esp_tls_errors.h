#ifndef HTTP_MOCK_ESP_TLS_ERRORS_H
#define HTTP_MOCK_ESP_TLS_ERRORS_H

/*
 * Заглушка esp_tls_errors.h для host-стенда Http (T121 П4): модуль различает
 * сбой резолва DNS по коду ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME. Значения
 * взяты из IDF 5.5.1 (components/esp-tls/esp_tls_errors.h).
 */
#define ESP_ERR_ESP_TLS_BASE 0x8000
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME (ESP_ERR_ESP_TLS_BASE + 0x01)
#define ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET (ESP_ERR_ESP_TLS_BASE + 0x02)
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST (ESP_ERR_ESP_TLS_BASE + 0x04)
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT (ESP_ERR_ESP_TLS_BASE + 0x06)

#endif  // HTTP_MOCK_ESP_TLS_ERRORS_H
