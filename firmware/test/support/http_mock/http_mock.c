/*
 * Мок esp_http_client для host-стенда Http (T121 П4).
 *
 * Сеть не поднимается: esp_http_client_init запоминает конфигурацию и отдаёт
 * непрозрачный handle, а последнюю TLS-ошибку стенд заказывает сам. Этого
 * хватает, чтобы проверить всю логику app_http.cpp без платы и без сети.
 */
#include "esp_http_client.h"

#include <stdlib.h>
#include <string.h>

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

void http_mock_reset(void)
{
    memset(&s_last_cfg, 0, sizeof(s_last_cfg));
    s_have_cfg = false;
    s_init_calls = 0;
    s_fail_next_init = false;
    s_tls_err = 0;
    s_tls_flags = 0;
    s_tls_error_calls = 0;
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
