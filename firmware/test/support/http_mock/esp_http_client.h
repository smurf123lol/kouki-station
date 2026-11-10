#ifndef HTTP_MOCK_ESP_HTTP_CLIENT_H
#define HTTP_MOCK_ESP_HTTP_CLIENT_H

/*
 * Заглушка esp_http_client.h для host-стенда Http (T121 П4).
 *
 * Повторяет только то, чем пользуется app_http.cpp: поля конфигурации
 * (url, timeout_ms, cert_pem, event_handler), событие ON_CONNECTED и
 * получение последней TLS-ошибки. Сеть не поднимается: esp_http_client_init
 * запоминает конфигурацию и отдаёт непрозрачный handle.
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTP_METHOD_GET = 1,
    HTTP_METHOD_POST,
} esp_http_client_method_t;

typedef struct esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    void *data;
    int data_len;
    void *user_data;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    const char *cert_pem;
    esp_err_t (*crt_bundle_attach)(void *conf);
    http_event_handle_cb event_handler;
    void *user_data;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client,
                                                       int *esp_tls_error_code,
                                                       int *esp_tls_flags);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

/* --- Управление моком из стенда --------------------------------------- */

/** Сброс: забыть последнюю конфигурацию, счётчики и заказанную TLS-ошибку. */
void http_mock_reset(void);

/** Конфигурация, с которой вызывали esp_http_client_init (NULL — не вызывали). */
const esp_http_client_config_t *http_mock_last_config(void);

/** Сколько раз вызывали esp_http_client_init. */
int http_mock_init_calls(void);

/** Следующий esp_http_client_init вернёт NULL (имитация нехватки памяти). */
void http_mock_fail_next_init(void);

/** Заказать значения, которые вернёт get_and_clear_last_tls_error. */
void http_mock_set_tls_error(int tls_err, int tls_flags);

/** Сколько раз спрашивали последнюю TLS-ошибку. */
int http_mock_tls_error_calls(void);

/** Вызвать сохранённый event_handler с событием ON_CONNECTED (как это делает IDF). */
void http_mock_fire_connected(void);

#ifdef __cplusplus
}
#endif

#endif  // HTTP_MOCK_ESP_HTTP_CLIENT_H
