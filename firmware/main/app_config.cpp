#include "app_config.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"

extern const char _binary_kouki_local_root_pem_start[] asm("_binary_kouki_local_root_pem_start");
extern const char _binary_kouki_local_root_pem_end[] asm("_binary_kouki_local_root_pem_end");

namespace kouki {
namespace {
const char *const TAG = "kouki.config";
}  // namespace

Config::Config()
    : m_cfg(),
      m_initialized(false),
      m_silent(false),
      m_wake_en(KOUKI_CONFIG_DEFAULT_WAKE_ENABLED != 0),
      m_wake_th(KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD)
{
    set_defaults();
}

void Config::set_defaults()
{
    memset(&m_cfg, 0, sizeof(m_cfg));
    strncpy(m_cfg.dev_source, KOUKI_CONFIG_DEFAULT_SOURCE, sizeof(m_cfg.dev_source) - 1);
    m_cfg.ota_int_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
#if CONFIG_KOUKI_SILENT_MODE
    m_cfg.silent = true;
#else
    m_cfg.silent = false;
#endif
    m_cfg.wake_en = (KOUKI_CONFIG_DEFAULT_WAKE_ENABLED != 0);
    m_cfg.wake_th = KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
    m_silent.store(m_cfg.silent, std::memory_order_release);
    m_wake_en.store(m_cfg.wake_en, std::memory_order_release);
    m_wake_th.store(m_cfg.wake_th, std::memory_order_release);
}

esp_err_t Config::init()
{
    set_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(KOUKI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "namespace «%s» не найден, конфигурация пуста", KOUKI_CONFIG_NVS_NAMESPACE);
        m_initialized = true;
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
        m_initialized = true;
        return ESP_OK;
    }
    m_cfg.cfg_ver = ver;

    size_t len = sizeof(m_cfg.wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", m_cfg.wifi_ssid, &len);

    len = sizeof(m_cfg.wifi_pass);
    nvs_get_str(handle, "wifi_pass", m_cfg.wifi_pass, &len);

    len = sizeof(m_cfg.srv_url);
    nvs_get_str(handle, "srv_url", m_cfg.srv_url, &len);

    len = sizeof(m_cfg.srv_token);
    nvs_get_str(handle, "srv_token", m_cfg.srv_token, &len);

    len = sizeof(m_cfg.dev_source);
    if (nvs_get_str(handle, "dev_source", m_cfg.dev_source, &len) != ESP_OK || m_cfg.dev_source[0] == '\0') {
        strncpy(m_cfg.dev_source, KOUKI_CONFIG_DEFAULT_SOURCE, sizeof(m_cfg.dev_source) - 1);
    }

    len = sizeof(m_cfg.ca_pem);
    if (nvs_get_str(handle, "ca_pem", m_cfg.ca_pem, &len) != ESP_OK) {
        m_cfg.ca_pem[0] = '\0';
    }

    uint32_t ota = 0;
    if (nvs_get_u32(handle, "ota_int_s", &ota) == ESP_OK && ota > 0) {
        m_cfg.ota_int_s = ota;
    } else {
        m_cfg.ota_int_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
    }

    uint8_t silent_val = 0;
    if (nvs_get_u8(handle, "silent", &silent_val) == ESP_OK) {
        m_cfg.silent = (silent_val != 0);
    }

    uint8_t wake_en_val = 0;
    if (nvs_get_u8(handle, "wake_en", &wake_en_val) == ESP_OK) {
        m_cfg.wake_en = (wake_en_val != 0);
    } else {
        m_cfg.wake_en = (KOUKI_CONFIG_DEFAULT_WAKE_ENABLED != 0);
    }

    uint8_t wake_th_val = 0;
    if (nvs_get_u8(handle, "wake_th", &wake_th_val) == ESP_OK &&
        wake_th_val >= KOUKI_CONFIG_MIN_WAKE_THRESHOLD &&
        wake_th_val <= KOUKI_CONFIG_MAX_WAKE_THRESHOLD) {
        m_cfg.wake_th = wake_th_val;
    } else {
        m_cfg.wake_th = KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
    }

    /* Миграция 10.11.2026: слайдер раньше назывался «Чувствительность» и писал
       свой процент прямо в порог, поэтому «максимум чувствительности» превращался
       в порог 95% и кодовое слово почти не срабатывало. Один раз переводим
       сохранённые значения >= 90 в осмысленный порог. */
    uint8_t wake_mig = 0;
    if (nvs_get_u8(handle, "wake_mig", &wake_mig) != ESP_OK || wake_mig != 1) {
        if (m_cfg.wake_th >= 90) {
            ESP_LOGW(TAG, "миграция порога wake: %u%% -> %u%%",
                     (unsigned)m_cfg.wake_th, (unsigned)KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD);
            m_cfg.wake_th = KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
            nvs_set_u8(handle, "wake_th", m_cfg.wake_th);
        }
        nvs_set_u8(handle, "wake_mig", 1);
        nvs_commit(handle);
    }

    m_silent.store(m_cfg.silent, std::memory_order_release);
    m_wake_en.store(m_cfg.wake_en, std::memory_order_release);
    m_wake_th.store(m_cfg.wake_th, std::memory_order_release);

    nvs_close(handle);
    m_initialized = true;
    ESP_LOGI(TAG, "конфигурация прочитана (полная: %s)", is_complete() ? "да" : "нет");
    return ESP_OK;
}

bool Config::is_complete() const
{
    if (m_cfg.cfg_ver != KOUKI_CONFIG_VERSION) {
        return false;
    }
    const size_t ssid_len = strlen(m_cfg.wifi_ssid);
    if (ssid_len == 0 || ssid_len > KOUKI_CONFIG_MAX_SSID_LEN) {
        return false;
    }
    const size_t url_len = strlen(m_cfg.srv_url);
    if (url_len == 0 || url_len > KOUKI_CONFIG_MAX_URL_LEN) {
        return false;
    }
#if !CONFIG_KOUKI_ALLOW_HTTP
    if (strncmp(m_cfg.srv_url, "https://", 8) != 0) {
        return false;
    }
#else
    if (strncmp(m_cfg.srv_url, "http://", 7) != 0 && strncmp(m_cfg.srv_url, "https://", 8) != 0) {
        return false;
    }
#endif
    const size_t token_len = strlen(m_cfg.srv_token);
    if (token_len < KOUKI_CONFIG_MIN_TOKEN_LEN || token_len > KOUKI_CONFIG_MAX_TOKEN_LEN) {
        return false;
    }
    return true;
}

const char *Config::wifi_ssid() const
{
    return m_cfg.wifi_ssid;
}

const char *Config::wifi_pass() const
{
    return m_cfg.wifi_pass;
}

const char *Config::srv_url() const
{
    return m_cfg.srv_url;
}

const char *Config::srv_token() const
{
    return m_cfg.srv_token;
}

const char *Config::dev_source() const
{
    return m_cfg.dev_source[0] ? m_cfg.dev_source : KOUKI_CONFIG_DEFAULT_SOURCE;
}

const char *Config::ca_pem() const
{
    return m_cfg.ca_pem;
}

const char *Config::effective_ca_pem() const
{
    if (m_cfg.ca_pem[0] != '\0' && strstr(m_cfg.ca_pem, "-----BEGIN CERTIFICATE-----") != NULL) {
        return m_cfg.ca_pem;
    }
    return _binary_kouki_local_root_pem_start;
}

uint32_t Config::ota_interval_s() const
{
    return m_cfg.ota_int_s > 0 ? m_cfg.ota_int_s : KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
}

esp_err_t Config::set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > KOUKI_CONFIG_MAX_SSID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass != NULL && strlen(pass) > KOUKI_CONFIG_MAX_PASS_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(m_cfg.wifi_ssid, ssid, sizeof(m_cfg.wifi_ssid) - 1);
    m_cfg.wifi_ssid[sizeof(m_cfg.wifi_ssid) - 1] = '\0';

    if (pass != NULL) {
        strncpy(m_cfg.wifi_pass, pass, sizeof(m_cfg.wifi_pass) - 1);
        m_cfg.wifi_pass[sizeof(m_cfg.wifi_pass) - 1] = '\0';
    } else {
        m_cfg.wifi_pass[0] = '\0';
    }

    return ESP_OK;
}

esp_err_t Config::set_url(const char *url)
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

    strncpy(m_cfg.srv_url, url, sizeof(m_cfg.srv_url) - 1);
    m_cfg.srv_url[sizeof(m_cfg.srv_url) - 1] = '\0';
    return ESP_OK;
}

esp_err_t Config::set_token(const char *token)
{
    if (token == NULL || strlen(token) < KOUKI_CONFIG_MIN_TOKEN_LEN || strlen(token) > KOUKI_CONFIG_MAX_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(m_cfg.srv_token, token, sizeof(m_cfg.srv_token) - 1);
    m_cfg.srv_token[sizeof(m_cfg.srv_token) - 1] = '\0';
    return ESP_OK;
}

esp_err_t Config::set_source(const char *source)
{
    if (source == NULL || strlen(source) == 0) {
        strncpy(m_cfg.dev_source, KOUKI_CONFIG_DEFAULT_SOURCE, sizeof(m_cfg.dev_source) - 1);
    } else {
        if (strlen(source) > KOUKI_CONFIG_MAX_SOURCE_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(m_cfg.dev_source, source, sizeof(m_cfg.dev_source) - 1);
        m_cfg.dev_source[sizeof(m_cfg.dev_source) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t Config::set_ca_pem(const char *pem)
{
    if (pem == NULL || pem[0] == '\0') {
        m_cfg.ca_pem[0] = '\0';
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

    strncpy(m_cfg.ca_pem, pem, sizeof(m_cfg.ca_pem) - 1);
    m_cfg.ca_pem[sizeof(m_cfg.ca_pem) - 1] = '\0';
    return ESP_OK;
}

esp_err_t Config::set_ota_interval(uint32_t interval_s)
{
    if (interval_s == 0) {
        interval_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
    }
    m_cfg.ota_int_s = interval_s;
    return ESP_OK;
}

esp_err_t Config::save()
{
    m_cfg.cfg_ver = KOUKI_CONFIG_VERSION;
    if (!is_complete()) {
        m_cfg.cfg_ver = 0;
        ESP_LOGW(TAG, "попытка сохранить неполную конфигурацию");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(KOUKI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "не удалось открыть NVS для записи: %s", esp_err_to_name(err));
        return err;
    }

    m_cfg.cfg_ver = KOUKI_CONFIG_VERSION;
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "cfg_ver", m_cfg.cfg_ver));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "wifi_ssid", m_cfg.wifi_ssid));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "wifi_pass", m_cfg.wifi_pass));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "srv_url", m_cfg.srv_url));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "srv_token", m_cfg.srv_token));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "dev_source", m_cfg.dev_source));
    if (m_cfg.ca_pem[0] != '\0') {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(handle, "ca_pem", m_cfg.ca_pem));
    } else {
        nvs_erase_key(handle, "ca_pem");
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(handle, "ota_int_s", m_cfg.ota_int_s));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "silent", m_cfg.silent ? 1 : 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "wake_en", m_cfg.wake_en ? 1 : 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(handle, "wake_th", m_cfg.wake_th));

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "конфигурация успешно сохранена в NVS");
    } else {
        ESP_LOGE(TAG, "ошибка nvs_commit: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t Config::erase()
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

bool Config::wake_enabled() const
{
    return m_wake_en.load(std::memory_order_acquire);
}

esp_err_t Config::set_wake_enabled(bool enabled)
{
    m_wake_en.store(enabled, std::memory_order_release);
    m_cfg.wake_en = enabled;
    return ESP_OK;
}

uint8_t Config::wake_threshold() const
{
    uint32_t th = static_cast<uint32_t>(m_wake_th.load(std::memory_order_acquire));
    return (th >= KOUKI_CONFIG_MIN_WAKE_THRESHOLD && th <= KOUKI_CONFIG_MAX_WAKE_THRESHOLD)
        ? static_cast<uint8_t>(th) : KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD;
}

esp_err_t Config::set_wake_threshold(uint8_t threshold)
{
    if (threshold < KOUKI_CONFIG_MIN_WAKE_THRESHOLD || threshold > KOUKI_CONFIG_MAX_WAKE_THRESHOLD) {
        return ESP_ERR_INVALID_ARG;
    }
    m_wake_th.store(threshold, std::memory_order_release);
    m_cfg.wake_th = threshold;
    return ESP_OK;
}

bool Config::is_silent() const
{
    return m_silent.load(std::memory_order_acquire);
}

void Config::set_silent(bool silent)
{
    m_silent.store(silent, std::memory_order_release);
    m_cfg.silent = silent;
}

void Config::format_masked(char *buf, size_t max_len) const
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
             (unsigned)m_cfg.cfg_ver,
             m_cfg.wifi_ssid[0] ? "***" : "(не задан)",
             m_cfg.wifi_pass[0] ? "***" : "(пустой)",
             m_cfg.srv_url[0] ? m_cfg.srv_url : "(не задан)",
             m_cfg.srv_token[0] ? "***" : "(не задан)",
             m_cfg.dev_source[0] ? m_cfg.dev_source : KOUKI_CONFIG_DEFAULT_SOURCE,
             m_cfg.ca_pem[0] ? "(пользовательский NVS)" : "(вшитый Caddy CA)",
             (unsigned)m_cfg.ota_int_s,
             m_cfg.silent ? "включен (тишина)" : "выключен (с речью)",
             m_cfg.wake_en ? "включен (\"эй гин\", отсечка 3.0с)" : "выключен",
             (unsigned)m_cfg.wake_th);
}

Config &config()
{
    // Один статический экземпляр на образ: без new/delete, без деструктора.
    static Config s_instance;
    return s_instance;
}

}  // namespace kouki

// ---------------------------------------------------------------------------
// Публичный C-фасад. Другого места сборки фасадов в П2 нет: main.c, FSM и UI
// продолжают звать kouki_config_*, а те делегируют в kouki::config().
// ---------------------------------------------------------------------------

extern "C" {

bool kouki_config_is_silent(void)
{
    return kouki::config().is_silent();
}

void kouki_config_set_silent(bool silent)
{
    kouki::config().set_silent(silent);
}

esp_err_t kouki_config_init(void)
{
    return kouki::config().init();
}

bool kouki_config_is_complete(void)
{
    return kouki::config().is_complete();
}

const char *kouki_config_get_wifi_ssid(void)
{
    return kouki::config().wifi_ssid();
}

const char *kouki_config_get_wifi_pass(void)
{
    return kouki::config().wifi_pass();
}

const char *kouki_config_get_srv_url(void)
{
    return kouki::config().srv_url();
}

const char *kouki_config_get_srv_token(void)
{
    return kouki::config().srv_token();
}

const char *kouki_config_get_dev_source(void)
{
    return kouki::config().dev_source();
}

const char *kouki_config_get_ca_pem(void)
{
    return kouki::config().ca_pem();
}

const char *kouki_config_get_effective_ca_pem(void)
{
    return kouki::config().effective_ca_pem();
}

uint32_t kouki_config_get_ota_interval_s(void)
{
    return kouki::config().ota_interval_s();
}

bool kouki_config_get_wake_enabled(void)
{
    return kouki::config().wake_enabled();
}

esp_err_t kouki_config_set_wake_enabled(bool enabled)
{
    return kouki::config().set_wake_enabled(enabled);
}

uint8_t kouki_config_get_wake_threshold(void)
{
    return kouki::config().wake_threshold();
}

esp_err_t kouki_config_set_wake_threshold(uint8_t threshold)
{
    return kouki::config().set_wake_threshold(threshold);
}

esp_err_t kouki_config_set_wifi(const char *ssid, const char *pass)
{
    return kouki::config().set_wifi(ssid, pass);
}

esp_err_t kouki_config_set_url(const char *url)
{
    return kouki::config().set_url(url);
}

esp_err_t kouki_config_set_token(const char *token)
{
    return kouki::config().set_token(token);
}

esp_err_t kouki_config_set_source(const char *source)
{
    return kouki::config().set_source(source);
}

esp_err_t kouki_config_set_ca_pem(const char *pem)
{
    return kouki::config().set_ca_pem(pem);
}

esp_err_t kouki_config_set_ota_interval(uint32_t interval_s)
{
    return kouki::config().set_ota_interval(interval_s);
}

esp_err_t kouki_config_save(void)
{
    return kouki::config().save();
}

esp_err_t kouki_config_erase(void)
{
    return kouki::config().erase();
}

void kouki_config_format_masked(char *buf, size_t max_len)
{
    kouki::config().format_masked(buf, max_len);
}

}  // extern "C"
