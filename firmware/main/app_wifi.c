#include "app_wifi.h"

#include <string.h>

#include "app_config.h"
#include "app_time.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "kouki.wifi";

#define CONNECTED_BIT BIT0
#define FAILED_BIT    BIT1

static EventGroupHandle_t s_events;
static bool s_connected;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "соединение потеряно, пробуем снова");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "адрес получен: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (event->ip_info.gw.addr != 0) {
            char gw_str[16] = {0};
            esp_ip4addr_ntoa(&event->ip_info.gw, gw_str, sizeof(gw_str));
            kouki_time_set_fallback_server(gw_str);
        }
        kouki_time_init(NULL);
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t kouki_wifi_connect(void)
{
    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL),
                        TAG, "регистрация WIFI_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL),
                        TAG, "регистрация IP_EVENT");

    if (!kouki_config_is_complete()) {
        ESP_LOGE(TAG, "конфигурация не готова: Wi-Fi не настроен");
        return ESP_ERR_INVALID_STATE;
    }

    const char *ssid = kouki_config_get_wifi_ssid();
    const char *pass = kouki_config_get_wifi_pass();
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pass != NULL && strlen(pass) > 0) {
        strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    /* T95: определяем обрыв AP за 3 с (минимум в IDF для STA) вместо стандартных 6 с */
    esp_err_t inactive_err = esp_wifi_set_inactive_time(WIFI_IF_STA, 3);
    if (inactive_err != ESP_OK) {
        ESP_LOGW(TAG, "не удалось установить inactive_time: %s", esp_err_to_name(inactive_err));
    }

    ESP_LOGI(TAG, "подключаюсь к Wi-Fi");
    const EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(CONFIG_KOUKI_WIFI_TIMEOUT_S * 1000));
    if ((bits & CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "сеть недоступна за %d с", CONFIG_KOUKI_WIFI_TIMEOUT_S);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool kouki_wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t kouki_wifi_get_info(char *ip_buf, size_t ip_len, int8_t *rssi)
{
    if (!s_connected) {
        if (ip_buf != NULL && ip_len > 0) {
            ip_buf[0] = '\0';
        }
        if (rssi != NULL) {
            *rssi = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (ip_buf != NULL && ip_len > 0) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_buf, ip_len);
        } else {
            ip_buf[0] = '\0';
        }
    }

    if (rssi != NULL) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            *rssi = ap_info.rssi;
        } else {
            *rssi = 0;
        }
    }

    return ESP_OK;
}
