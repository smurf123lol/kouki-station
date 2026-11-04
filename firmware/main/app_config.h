#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define KOUKI_CONFIG_NVS_NAMESPACE "kouki_config"
#define KOUKI_CONFIG_VERSION       1

#define KOUKI_CONFIG_MAX_SSID_LEN   32
#define KOUKI_CONFIG_MAX_PASS_LEN   64
#define KOUKI_CONFIG_MAX_URL_LEN    128
#define KOUKI_CONFIG_MAX_TOKEN_LEN  256
#define KOUKI_CONFIG_MAX_SOURCE_LEN 32

#define KOUKI_CONFIG_MIN_TOKEN_LEN  8

#define KOUKI_CONFIG_DEFAULT_SOURCE       "station"
#define KOUKI_CONFIG_DEFAULT_OTA_INTERVAL 43200

typedef struct {
    uint8_t cfg_ver;
    char wifi_ssid[KOUKI_CONFIG_MAX_SSID_LEN + 1];
    char wifi_pass[KOUKI_CONFIG_MAX_PASS_LEN + 1];
    char srv_url[KOUKI_CONFIG_MAX_URL_LEN + 1];
    char srv_token[KOUKI_CONFIG_MAX_TOKEN_LEN + 1];
    char dev_source[KOUKI_CONFIG_MAX_SOURCE_LEN + 1];
    uint32_t ota_int_s;
} kouki_config_t;

/**
 * Инициализирует модуль конфигурации: читает настройки из NVS.
 * Если nvs_flash_init() уже выполнен, читает namespace kouki_config.
 */
esp_err_t kouki_config_init(void);

/**
 * Проверяет, заполнена ли конфигурация полностью и валидна ли она.
 * Обязательные поля:
 * - cfg_ver == 1
 * - wifi_ssid не пуст (1..32)
 * - srv_url начинается с http:// или https:// (<= 128)
 * - srv_token длиной >= 8 (<= 256)
 */
bool kouki_config_is_complete(void);

/* Геттеры (возвращают указатели на внутренний буфер) */
const char *kouki_config_get_wifi_ssid(void);
const char *kouki_config_get_wifi_pass(void);
const char *kouki_config_get_srv_url(void);
const char *kouki_config_get_srv_token(void);
const char *kouki_config_get_dev_source(void);
uint32_t kouki_config_get_ota_interval_s(void);

/* Сеттеры / валидаторы */
esp_err_t kouki_config_set_wifi(const char *ssid, const char *pass);
esp_err_t kouki_config_set_url(const char *url);
esp_err_t kouki_config_set_token(const char *token);
esp_err_t kouki_config_set_source(const char *source);
esp_err_t kouki_config_set_ota_interval(uint32_t interval_s);

/**
 * Сохраняет текущую конфигурацию из памяти в NVS и вызывает nvs_commit.
 * Выставляет cfg_ver = 1.
 */
esp_err_t kouki_config_save(void);

/**
 * Стирает весь namespace kouki_config в NVS и сбрасывает значения в памяти.
 */
esp_err_t kouki_config_erase(void);

/**
 * Форматирует сводку конфигурации с маскированием секретов (SSID, пароль, токен маскируются ***).
 */
void kouki_config_format_masked(char *buf, size_t max_len);

#endif // APP_CONFIG_H
