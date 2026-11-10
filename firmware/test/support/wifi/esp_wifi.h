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

typedef struct {
    wifi_sta_config_t sta;
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
