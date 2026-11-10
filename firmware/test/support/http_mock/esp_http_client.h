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
    /* Ниже — поля, которыми пользуется модуль OTA (T121 П8). */
    bool keep_alive_enable;
    bool skip_cert_common_name_check;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client,
                                                       int *esp_tls_error_code,
                                                       int *esp_tls_flags);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

/* --- Поверхность модуля OTA (T121 П8): GET /api/firmware -------------- */

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);

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

/* --- Управление GET-ответом (модуль OTA, T121 П8) --------------------- */

/** Заказать статус и тело ответа на GET /api/firmware. */
void http_mock_set_response(int status_code, const char *body);

/** Заказать ошибку следующего esp_http_client_open (ESP_OK — успех). */
void http_mock_fail_open(esp_err_t err);

/** Значение заголовка, который выставил код (NULL — заголовок не ставили). */
const char *http_mock_header(const char *key);

/** Сколько раз читали тело ответа. */
int http_mock_read_calls(void);

/** Счётчики открытия/закрытия: стенд видит, что клиент закрыли после ответа. */
int http_mock_open_calls(void);
int http_mock_close_calls(void);

#ifdef __cplusplus
}
#endif

#endif  // HTTP_MOCK_ESP_HTTP_CLIENT_H
