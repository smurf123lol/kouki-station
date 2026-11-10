/*
 * Мок esp_http_client для host-стенда Http (T121 П4) и Ota (T121 П8).
 *
 * Сеть не поднимается: esp_http_client_init запоминает конфигурацию и отдаёт
 * непрозрачный handle, а последнюю TLS-ошибку стенд заказывает сам. Этого
 * хватает, чтобы проверить всю логику app_http.cpp без платы и без сети.
 *
 * Для модуля OTA (П8) мок дополнительно умеет отдать ответ GET /api/firmware:
 * статус, заголовки и тело задаёт тест, чтение тела и закрытие считаются.
 */
#include "esp_http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MOCK_MAX_HEADERS 4

typedef struct {
    char key[32];
    char value[320];
} http_mock_header_t;

struct esp_http_client {
    int unused;
};

static esp_http_client_config_t s_last_cfg;
static bool s_have_cfg = false;
static int s_init_calls = 0;
static bool s_fail_next_init = false;
static int s_tls_err = 0;
static int s_tls_flags = 0;
static int s_tls_error_calls = 0;

static int s_status_code = 200;
static char s_body[1024];
static esp_err_t s_open_err = ESP_OK;
static int s_read_calls = 0;
static int s_open_calls = 0;
static int s_close_calls = 0;
static http_mock_header_t s_headers[HTTP_MOCK_MAX_HEADERS];
static int s_header_count = 0;

void http_mock_reset(void)
{
    memset(&s_last_cfg, 0, sizeof(s_last_cfg));
    s_have_cfg = false;
    s_init_calls = 0;
    s_fail_next_init = false;
    s_tls_err = 0;
    s_tls_flags = 0;
    s_tls_error_calls = 0;
    s_status_code = 200;
    s_body[0] = '\0';
    s_open_err = ESP_OK;
    s_read_calls = 0;
    s_open_calls = 0;
    s_close_calls = 0;
    memset(s_headers, 0, sizeof(s_headers));
    s_header_count = 0;
}

const esp_http_client_config_t *http_mock_last_config(void)
{
    return s_have_cfg ? &s_last_cfg : NULL;
}

int http_mock_init_calls(void)
{
    return s_init_calls;
}

void http_mock_fail_next_init(void)
{
    s_fail_next_init = true;
}

void http_mock_set_tls_error(int tls_err, int tls_flags)
{
    s_tls_err = tls_err;
    s_tls_flags = tls_flags;
}

int http_mock_tls_error_calls(void)
{
    return s_tls_error_calls;
}

void http_mock_fire_connected(void)
{
    if (!s_have_cfg || s_last_cfg.event_handler == NULL) {
        return;
    }
    esp_http_client_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.event_id = HTTP_EVENT_ON_CONNECTED;
    evt.user_data = s_last_cfg.user_data;
    (void)s_last_cfg.event_handler(&evt);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    s_init_calls++;
    if (s_fail_next_init) {
        s_fail_next_init = false;
        return NULL;
    }
    if (config != NULL) {
        s_last_cfg = *config;
        s_have_cfg = true;
    }
    struct esp_http_client *client = calloc(1, sizeof(*client));
    return client;
}

esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client,
                                                       int *esp_tls_error_code,
                                                       int *esp_tls_flags)
{
    (void)client;
    s_tls_error_calls++;
    if (esp_tls_error_code != NULL) {
        *esp_tls_error_code = s_tls_err;
    }
    if (esp_tls_flags != NULL) {
        *esp_tls_flags = s_tls_flags;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    free(client);
    return ESP_OK;
}

/* --- Поверхность модуля OTA (T121 П8): GET /api/firmware --------------- */

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    (void)client;
    if (key == NULL || value == NULL || s_header_count >= HTTP_MOCK_MAX_HEADERS) {
        return ESP_ERR_INVALID_ARG;
    }
    http_mock_header_t *h = &s_headers[s_header_count++];
    snprintf(h->key, sizeof(h->key), "%s", key);
    snprintf(h->value, sizeof(h->value), "%s", value);
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    (void)client;
    (void)write_len;
    s_open_calls++;
    return s_open_err;
}

int esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    (void)client;
    return (int)strlen(s_body);
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return s_status_code;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    (void)client;
    s_read_calls++;
    if (buffer == NULL || len <= 0) {
        return -1;
    }
    const size_t body_len = strlen(s_body);
    if (body_len == 0) {
        return 0;
    }
    const size_t n = (body_len < (size_t)len) ? body_len : (size_t)len;
    memcpy(buffer, s_body, n);
    return (int)n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    (void)client;
    s_close_calls++;
    return ESP_OK;
}

void http_mock_set_response(int status_code, const char *body)
{
    s_status_code = status_code;
    if (body != NULL) {
        snprintf(s_body, sizeof(s_body), "%s", body);
    } else {
        s_body[0] = '\0';
    }
}

void http_mock_fail_open(esp_err_t err)
{
    s_open_err = err;
}

const char *http_mock_header(const char *key)
{
    if (key == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_header_count; i++) {
        if (strcmp(s_headers[i].key, key) == 0) {
            return s_headers[i].value;
        }
    }
    return NULL;
}

int http_mock_read_calls(void)
{
    return s_read_calls;
}

int http_mock_open_calls(void)
{
    return s_open_calls;
}

int http_mock_close_calls(void)
{
    return s_close_calls;
}
