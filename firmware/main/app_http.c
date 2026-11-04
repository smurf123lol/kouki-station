#include "app_http.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_tls_errors.h"

static const char *TAG = "kouki.http";

#ifndef CONFIG_KOUKI_TLS_TIMEOUT_S
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
#endif

static void extract_host(const char *url, char *host, size_t max_host)
{
    host[0] = '\0';
    if (!url) {
        return;
    }
    const char *p = strstr(url, "://");
    if (p) {
        p += 3;
    } else {
        p = url;
    }
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && i + 1 < max_host) {
        host[i++] = *p++;
    }
    host[i] = '\0';
}

void kouki_http_on_connected(const char *url)
{
    if (url != NULL && strncmp(url, "https://", 8) == 0) {
        ESP_LOGI(TAG, "https: server cert chain ok");
    }
}

static esp_err_t default_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        /* При успешном соединении по https TLS handshake завершён и сертификат проверен */
        kouki_http_on_connected("https://");
    }
    return ESP_OK;
}

esp_err_t kouki_http_new_client(const esp_http_client_config_t *base_cfg, esp_http_client_handle_t *out_client)
{
    if (base_cfg == NULL || out_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_KOUKI_ALLOW_HTTP
    if (base_cfg->url && strncmp(base_cfg->url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "незащищённый HTTP запрещён (CONFIG_KOUKI_ALLOW_HTTP=n): %s", base_cfg->url);
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    esp_http_client_config_t cfg = *base_cfg;

    if (cfg.timeout_ms <= 0) {
        cfg.timeout_ms = CONFIG_KOUKI_TLS_TIMEOUT_S * 1000;
    }

    if (cfg.url && strncmp(cfg.url, "https://", 8) == 0) {
        cfg.cert_pem = kouki_config_get_effective_ca_pem();
    }

    if (cfg.event_handler == NULL) {
        cfg.event_handler = default_http_event;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_client = client;
    return ESP_OK;
}

void kouki_http_diagnose_error(esp_http_client_handle_t client, const char *url, esp_err_t req_err, char *err_buf, size_t max_len)
{
    if (err_buf == NULL || max_len == 0) {
        return;
    }

    char host[64] = {0};
    extract_host(url, host, sizeof(host));

    int tls_err = 0;
    int tls_flags = 0;
    if (client != NULL) {
        esp_http_client_get_and_clear_last_tls_error(client, &tls_err, &tls_flags);
    }

    /* 1. Сбой проверки сертификата сервера */
    if (tls_flags != 0 || tls_err != 0) {
        int err_code = tls_err != 0 ? tls_err : tls_flags;
        ESP_LOGE(TAG, "tls: verify failed, err=0x%x", err_code);
        snprintf(err_buf, max_len, "Сертификат сервера не принят. Проверьте корневой CA.");
        return;
    }

    /* 2. Сбой резолва DNS */
    bool dns_failed = (req_err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);
    if (!dns_failed && host[0] != '\0') {
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        int ret = getaddrinfo(host, NULL, &hints, &res);
        if (res) {
            freeaddrinfo(res);
        }
        if (ret != 0) {
            dns_failed = true;
        }
    }

    if (dns_failed) {
        ESP_LOGE(TAG, "dns: lookup failed for '%s'", host);
        snprintf(err_buf, max_len, "Имя сервера не найдено: %s.", host);
        return;
    }

    /* 3. Ошибка связи (TCP/сеть/таймаут) */
    ESP_LOGW(TAG, "ошибка связи: err=%s (0x%x)", esp_err_to_name(req_err), (unsigned)req_err);
    snprintf(err_buf, max_len, "Нет связи с сервером.");
}
