#pragma once

/*
 * Заглушка esp_wifi.h для host-стенда Wifi (T121 П7).
 *
 * Повторяет только то, чем пользуется app_wifi.cpp: типы событий, структуры
 * конфигурации станции и функции старта/подключения. Сами функции живут в
 * wifi_mock.c и ведут журнал, чтобы стенд видел порядок вызовов.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA,
    WIFI_MODE_AP,
    WIFI_MODE_APSTA,
} wifi_mode_t;

typedef enum {
    WIFI_IF_STA = 0,
    WIFI_IF_AP,
} wifi_interface_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
} wifi_auth_mode_t;

/** База события Wi-Fi (в IDF — esp_event base). */
extern const char *WIFI_EVENT;

/** События Wi-Fi: значения как в IDF. */
typedef enum {
    WIFI_EVENT_WIFI_READY = 0,
    WIFI_EVENT_SCAN_DONE,
    WIFI_EVENT_STA_START,
    WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_CONNECTED,
    WIFI_EVENT_STA_DISCONNECTED,
} wifi_event_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct {
        wifi_auth_mode_t authmode;
    } threshold;
    bool bssid_set;
    uint8_t bssid[6];
} wifi_sta_config_t;

/* Типы ниже нужны только для того, чтобы wifi_ap_config_t имел ТУ ЖЕ
 * раскладку полей, что в IDF (esp_wifi_types_generic.h:525-544). Ни один
 * тест их значения не читает: SoftAP-портал выставляет ssid/password/
 * authmode/max_connection, остальное остаётся нулями. */
typedef enum {
    WIFI_CIPHER_TYPE_NONE = 0,
    WIFI_CIPHER_TYPE_WEP40,
    WIFI_CIPHER_TYPE_TKIP,
    WIFI_CIPHER_TYPE_CCMP,
} wifi_cipher_type_t;

typedef struct {
    bool capable;
    bool required;
} wifi_pmf_config_t;

typedef enum {
    WPA3_SAE_PWE_UNSPECIFIED = 0,
    WPA3_SAE_PWE_HUNT_AND_PECK,
    WPA3_SAE_PWE_HASH_TO_ELEMENT,
    WPA3_SAE_PWE_BOTH,
} wifi_sae_pwe_method_t;

typedef struct {
    uint16_t period;
    bool protected_keep_alive;
} wifi_bss_max_idle_config_t;

/**
 * Конфигурация точки доступа (SoftAP портала настройки, T121 П11).
 *
 * Пункт 33 ревью П11: структура описана ЦЕЛИКОМ, как в IDF, а не до
 * beacon_interval. Хвост ниже тестами не используется, но без него мок
 * молча расходится с реальной прошивкой по sizeof, и любая будущая проверка
 * смещений полей после beacon_interval дала бы ложный результат.
 */
typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    uint8_t ssid_len;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    uint8_t ssid_hidden;
    uint8_t max_connection;
    uint16_t beacon_interval;
    uint8_t csa_count;
    uint8_t dtim_period;
    wifi_cipher_type_t pairwise_cipher;
    bool ftm_responder;
    wifi_pmf_config_t pmf_cfg;
    wifi_sae_pwe_method_t sae_pwe_h2e;
    uint8_t transition_disable;
    uint8_t sae_ext;
    wifi_bss_max_idle_config_t bss_max_idle_cfg;
    uint16_t gtk_rekey_interval;
} wifi_ap_config_t;

typedef struct {
    wifi_sta_config_t sta;
    wifi_ap_config_t ap;
} wifi_config_t;

typedef struct {
    int8_t rssi;
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;

typedef struct {
    int event_size;
    int static_rx_buf_num;
    int nvs_enable;
} wifi_init_config_t;

#define WIFI_INIT_CONFIG_DEFAULT() {.event_size = 32, .static_rx_buf_num = 10, .nvs_enable = 1}

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_inactive_time(wifi_interface_t interface, uint16_t sec);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);

#ifdef __cplusplus
}
#endif
