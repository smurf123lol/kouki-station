#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"

extern const char _binary_kouki_local_root_pem_start[] asm("_binary_kouki_local_root_pem_start");
extern const char _binary_kouki_local_root_pem_end[] asm("_binary_kouki_local_root_pem_end");

static const char *TAG __attribute__((unused)) = "kouki.config";

static kouki_config_t s_cfg;
static bool s_initialized = false;
static atomic_bool s_atomic_silent;
static atomic_bool s_atomic_wake_en;
static atomic_uint_fast32_t s_atomic_wake_th;

static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strncpy(s_cfg.dev_source, KOUKI_CONFIG_DEFAULT_SOURCE, sizeof(s_cfg.dev_source) - 1);
    s_cfg.ota_int_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
#if CONFIG_KOUKI_SILENT_MODE
    s_cfg.silent = true;
#else
    s_cfg.silent = false;
#endif
    s_cfg.wake_en = (KOUKI_CONFIG_DEFAULT_WAKE_ENABLED != 0);
    s_cfg.wake_th = KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
    atomic_store_explicit(&s_atomic_silent, s_cfg.silent, memory_order_release);
    atomic_store_explicit(&s_atomic_wake_en, s_cfg.wake_en, memory_order_release);
    atomic_store_explicit(&s_atomic_wake_th, s_cfg.wake_th, memory_order_release);
}

esp_err_t kouki_config_init(void)
{
    set_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(KOUKI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "namespace «%s» не найден, конфигурация пуста", KOUKI_CONFIG_NVS_NAMESPACE);
        s_initialized = true;
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "не удалось открыть NVS «%s»: %s", KOUKI_CONFIG_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    uint8_t ver = 0;
    err = nvs_get_u8(handle, "cfg_ver", &ver);
    if (err != ESP_OK || ver != KOUKI_CONFIG_VERSION) {
        ESP_LOGI(TAG, "версия схемы NVS отсутствует или не равна %u (найдено: %u)",
                 (unsigned)KOUKI_CONFIG_VERSION, (unsigned)ver);
        nvs_close(handle);
        s_initialized = true;
        return ESP_OK;
    }
    s_cfg.cfg_ver = ver;

    size_t len = sizeof(s_cfg.wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", s_cfg.wifi_ssid, &len);

    len = sizeof(s_cfg.wifi_pass);
    nvs_get_str(handle, "wifi_pass", s_cfg.wifi_pass, &len);

    len = sizeof(s_cfg.srv_url);
    nvs_get_str(handle, "srv_url", s_cfg.srv_url, &len);

    len = sizeof(s_cfg.srv_token);
    nvs_get_str(handle, "srv_token", s_cfg.srv_token, &len);

    len = sizeof(s_cfg.dev_source);
    if (nvs_get_str(handle, "dev_source", s_cfg.dev_source, &len) != ESP_OK || s_cfg.dev_source[0] == '\0') {
        strncpy(s_cfg.dev_source, KOUKI_CONFIG_DEFAULT_SOURCE, sizeof(s_cfg.dev_source) - 1);
    }

    len = sizeof(s_cfg.ca_pem);
    if (nvs_get_str(handle, "ca_pem", s_cfg.ca_pem, &len) != ESP_OK) {
        s_cfg.ca_pem[0] = '\0';
    }

    uint32_t ota = 0;
    if (nvs_get_u32(handle, "ota_int_s", &ota) == ESP_OK && ota > 0) {
        s_cfg.ota_int_s = ota;
    } else {
        s_cfg.ota_int_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
    }

    uint8_t silent_val = 0;
    if (nvs_get_u8(handle, "silent", &silent_val) == ESP_OK) {
        s_cfg.silent = (silent_val != 0);
    }

    uint8_t wake_en_val = 0;
    if (nvs_get_u8(handle, "wake_en", &wake_en_val) == ESP_OK) {
        s_cfg.wake_en = (wake_en_val != 0);
    } else {
        s_cfg.wake_en = (KOUKI_CONFIG_DEFAULT_WAKE_ENABLED != 0);
    }

    uint8_t wake_th_val = 0;
    if (nvs_get_u8(handle, "wake_th", &wake_th_val) == ESP_OK &&
        wake_th_val >= KOUKI_CONFIG_MIN_WAKE_THRESHOLD &&
        wake_th_val <= KOUKI_CONFIG_MAX_WAKE_THRESHOLD) {
        s_cfg.wake_th = wake_th_val;
    } else {
        s_cfg.wake_th = KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
    }

    /* Миграция 10.11.2026: слайдер раньше назывался «Чувствительность» и писал
       свой процент прямо в порог, поэтому «максимум чувствительности» превращался
       в порог 95% и кодовое слово почти не срабатывало. Один раз переводим
       сохранённые значения >= 90 в осмысленный порог. */
    uint8_t wake_mig = 0;
    if (nvs_get_u8(handle, "wake_mig", &wake_mig) != ESP_OK || wake_mig != 1) {
        if (s_cfg.wake_th >= 90) {
            ESP_LOGW(TAG, "миграция порога wake: %u%% -> %u%%",
                     (unsigned)s_cfg.wake_th, (unsigned)KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD);
            s_cfg.wake_th = KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
            nvs_set_u8(handle, "wake_th", s_cfg.wake_th);
        }
        nvs_set_u8(handle, "wake_mig", 1);
        nvs_commit(handle);
    }

    atomic_store_explicit(&s_atomic_silent, s_cfg.silent, memory_order_release);
    atomic_store_explicit(&s_atomic_wake_en, s_cfg.wake_en, memory_order_release);
    atomic_store_explicit(&s_atomic_wake_th, s_cfg.wake_th, memory_order_release);

    nvs_close(handle);
    s_initialized = true;
    ESP_LOGI(TAG, "конфигурация прочитана (полная: %s)", kouki_config_is_complete() ? "да" : "нет");
    return ESP_OK;
}

bool kouki_config_is_complete(void)
{
    if (s_cfg.cfg_ver != KOUKI_CONFIG_VERSION) {
        return false;
    }
    const size_t ssid_len = strlen(s_cfg.wifi_ssid);
    if (ssid_len == 0 || ssid_len > KOUKI_CONFIG_MAX_SSID_LEN) {
        return false;
    }
    const size_t url_len = strlen(s_cfg.srv_url);
    if (url_len == 0 || url_len > KOUKI_CONFIG_MAX_URL_LEN) {
        return false;
    }
#if !CONFIG_KOUKI_ALLOW_HTTP
    if (strncmp(s_cfg.srv_url, "https://", 8) != 0) {
        return false;
    }
#else
    if (strncmp(s_cfg.srv_url, "http://", 7) != 0 && strncmp(s_cfg.srv_url, "https://", 8) != 0) {
        return false;
    }
#endif
    const size_t token_len = strlen(s_cfg.srv_token);
    if (token_len < KOUKI_CONFIG_MIN_TOKEN_LEN || token_len > KOUKI_CONFIG_MAX_TOKEN_LEN) {
        return false;
    }
    return true;
}

const char *kouki_config_get_wifi_ssid(void)
{
    return s_cfg.wifi_ssid;
}

const char *kouki_config_get_wifi_pass(void)
{
    return s_cfg.wifi_pass;
}

const char *kouki_config_get_srv_url(void)
{
    return s_cfg.srv_url;
}

const char *kouki_config_get_srv_token(void)
{
    return s_cfg.srv_token;
}

const char *kouki_config_get_dev_source(void)
{
    return s_cfg.dev_source[0] ? s_cfg.dev_source : KOUKI_CONFIG_DEFAULT_SOURCE;
}

const char *kouki_config_get_ca_pem(void)
{
    return s_cfg.ca_pem;
}

const char *kouki_config_get_effective_ca_pem(void)
{
    if (s_cfg.ca_pem[0] != '\0' && strstr(s_cfg.ca_pem, "-----BEGIN CERTIFICATE-----") != NULL) {
        return s_cfg.ca_pem;
    }
    return _binary_kouki_local_root_pem_start;
}

uint32_t kouki_config_get_ota_interval_s(void)
{
    return s_cfg.ota_int_s > 0 ? s_cfg.ota_int_s : KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
}

esp_err_t kouki_config_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > KOUKI_CONFIG_MAX_SSID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass != NULL && strlen(pass) > KOUKI_CONFIG_MAX_PASS_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
    s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = '\0';

    if (pass != NULL) {
        strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
        s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = '\0';
    } else {
        s_cfg.wifi_pass[0] = '\0';
    }

    return ESP_OK;
}

esp_err_t kouki_config_set_url(const char *url)
{
    if (url == NULL || strlen(url) == 0 || strlen(url) > KOUKI_CONFIG_MAX_URL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_KOUKI_ALLOW_HTTP
    if (strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
#else
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
#endif

    strncpy(s_cfg.srv_url, url, sizeof(s_cfg.srv_url) - 1);
    s_cfg.srv_url[sizeof(s_cfg.srv_url) - 1] = '\0';
    return ESP_OK;
}

esp_err_t kouki_config_set_token(const char *token)
{
    if (token == NULL || strlen(token) < KOUKI_CONFIG_MIN_TOKEN_LEN || strlen(token) > KOUKI_CONFIG_MAX_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_cfg.srv_token, token, sizeof(s_cfg.srv_token) - 1);
    s_cfg.srv_token[sizeof(s_cfg.srv_token) - 1] = '\0';
    return ESP_OK;
}

esp_err_t kouki_config_set_source(const char *source)
{
    if (source == NULL || strlen(source) == 0) {
        strncpy(s_cfg.dev_source, KOUKI_CONFIG_DEFAULT_SOURCE, sizeof(s_cfg.dev_source) - 1);
    } else {
        if (strlen(source) > KOUKI_CONFIG_MAX_SOURCE_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(s_cfg.dev_source, source, sizeof(s_cfg.dev_source) - 1);
        s_cfg.dev_source[sizeof(s_cfg.dev_source) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t kouki_config_set_ca_pem(const char *pem)
{
    if (pem == NULL || pem[0] == '\0') {
        s_cfg.ca_pem[0] = '\0';
        return ESP_OK;
    }
    if (strlen(pem) > KOUKI_CONFIG_MAX_CA_PEM_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(pem, "-----BEGIN CERTIFICATE-----") == NULL ||
        strstr(pem, "-----END CERTIFICATE-----") == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1);
    mbedtls_x509_crt_free(&crt);
    if (ret != 0) {
        ESP_LOGW(TAG, "не удалось разобрать PEM сертификата: -0x%04x", -ret);
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_cfg.ca_pem, pem, sizeof(s_cfg.ca_pem) - 1);
    s_cfg.ca_pem[sizeof(s_cfg.ca_pem) - 1] = '\0';
    return ESP_OK;
}

esp_err_t kouki_config_set_ota_interval(uint32_t interval_s)
{
    if (interval_s == 0) {
        interval_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
    }
    s_cfg.ota_int_s = interval_s;
    return ESP_OK;
}

esp_err_t kouki_config_save(void)
{
    s_cfg.cfg_ver = KOUKI_CONFIG_VERSION;
    if (!kouki_config_is_complete()) {
        s_cfg.cfg_ver = 0;
        ESP_LOGW(TAG, "попытка сохранить неполную конфигурацию");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(KOUKI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "не удалось открыть NVS для записи: %s", esp_err_to_name(err));
        return err;
    }

    s_cfg.cfg_ver = KOUKI_CONFIG_VERSION;
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "cfg_ver", s_cfg.cfg_ver));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "wifi_ssid", s_cfg.wifi_ssid));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "wifi_pass", s_cfg.wifi_pass));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "srv_url", s_cfg.srv_url));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "srv_token", s_cfg.srv_token));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "dev_source", s_cfg.dev_source));
    if (s_cfg.ca_pem[0] != '\0') {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "ca_pem", s_cfg.ca_pem));
    } else {
        nvs_erase_key(handle, "ca_pem");
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(handle, "ota_int_s", s_cfg.ota_int_s));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "silent", s_cfg.silent ? 1 : 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "wake_en", s_cfg.wake_en ? 1 : 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "wake_th", s_cfg.wake_th));

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "конфигурация успешно сохранена в NVS");
    } else {
        ESP_LOGE(TAG, "ошибка nvs_commit: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t kouki_config_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KOUKI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    set_defaults();
    ESP_LOGI(TAG, "конфигурация очищена из NVS");
    return ESP_OK;
}

bool kouki_config_get_wake_enabled(void)
{
    return atomic_load_explicit(&s_atomic_wake_en, memory_order_acquire);
}

esp_err_t kouki_config_set_wake_enabled(bool enabled)
{
    atomic_store_explicit(&s_atomic_wake_en, enabled, memory_order_release);
    s_cfg.wake_en = enabled;
    return ESP_OK;
}

uint8_t kouki_config_get_wake_threshold(void)
{
    uint32_t th = (uint32_t)atomic_load_explicit(&s_atomic_wake_th, memory_order_acquire);
    return (th >= KOUKI_CONFIG_MIN_WAKE_THRESHOLD && th <= KOUKI_CONFIG_MAX_WAKE_THRESHOLD)
        ? (uint8_t)th : KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
}

esp_err_t kouki_config_set_wake_threshold(uint8_t threshold)
{
    if (threshold < KOUKI_CONFIG_MIN_WAKE_THRESHOLD || threshold > KOUKI_CONFIG_MAX_WAKE_THRESHOLD) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store_explicit(&s_atomic_wake_th, threshold, memory_order_release);
    s_cfg.wake_th = threshold;
    return ESP_OK;
}

bool kouki_config_is_silent(void)
{
    return atomic_load_explicit(&s_atomic_silent, memory_order_acquire);
}

void kouki_config_set_silent(bool silent)
{
    atomic_store_explicit(&s_atomic_silent, silent, memory_order_release);
    s_cfg.silent = silent;
}

void kouki_config_format_masked(char *buf, size_t max_len)
{
    if (buf == NULL || max_len == 0) {
        return;
    }
    snprintf(buf, max_len,
             "cfg_ver: %u\n"
             "wifi_ssid: %s\n"
             "wifi_pass: %s\n"
             "srv_url: %s\n"
             "srv_token: %s\n"
             "dev_source: %s\n"
             "ca_pem: %s\n"
             "ota_int_s: %u\n"
             "silent: %s\n"
             "wake_en: %s\n"
             "wake_th: %u\n",
             (unsigned)s_cfg.cfg_ver,
             s_cfg.wifi_ssid[0] ? "***" : "(не задан)",
             s_cfg.wifi_pass[0] ? "***" : "(пустой)",
             s_cfg.srv_url[0] ? s_cfg.srv_url : "(не задан)",
             s_cfg.srv_token[0] ? "***" : "(не задан)",
             s_cfg.dev_source[0] ? s_cfg.dev_source : KOUKI_CONFIG_DEFAULT_SOURCE,
             s_cfg.ca_pem[0] ? "(пользовательский NVS)" : "(вшитый Caddy CA)",
             (unsigned)s_cfg.ota_int_s,
             s_cfg.silent ? "включен (тишина)" : "выключен (с речью)",
             s_cfg.wake_en ? "включен (\"эй гин\", отсечка 3.0с)" : "выключен",
             (unsigned)s_cfg.wake_th);
}
