#include "app_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "kouki.wifi";

#define CONNECTED_BIT BIT0
#define FAILED_BIT    BIT1

static EventGroupHandle_t s_events;
static bool s_connected;

// Повторы идут с растущей паузой: при выключенном роутере плата не крутит попытки
// в цикле, а ждёт 1, 2, 4, 8, 16 секунд (дальше — каждые 16). Счётчик сбрасывается,
// когда адрес получен.
static const uint32_t RETRY_MAX_MS = 16000;
static uint32_t s_retry_delay_ms = 1000;
static uint32_t s_retry_count;
static esp_timer_handle_t s_retry_timer;

// Пауза между попытками — не vTaskDelay в обработчике события (он блокирует цикл
// событий), а одноразовый таймер: обработчик остаётся быстрым.
static void on_retry_timer(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_retry_count++;
        uint32_t delay_ms = s_retry_delay_ms;
        s_retry_delay_ms = delay_ms >= RETRY_MAX_MS ? RETRY_MAX_MS : delay_ms * 2;
        ESP_LOGW(TAG, "соединение потеряно (попытка %u), повтор через %u мс",
                 (unsigned)s_retry_count, (unsigned)delay_ms);
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
        } else {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "адрес получен: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_retry_count = 0;
        s_retry_delay_ms = 1000;
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

    if (!s_retry_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &on_retry_timer,
            .name = "wifi-retry",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "esp_timer_create");
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL),
                        TAG, "регистрация WIFI_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL),
                        TAG, "регистрация IP_EVENT");

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, CONFIG_KOUKI_WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, CONFIG_KOUKI_WIFI_PASSWORD, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = strlen(CONFIG_KOUKI_WIFI_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    ESP_LOGI(TAG, "подключаюсь к «%s»", CONFIG_KOUKI_WIFI_SSID);
    const EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(CONFIG_KOUKI_WIFI_TIMEOUT_S * 1000));
    if ((bits & CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "сеть «%s» недоступна за %d с", CONFIG_KOUKI_WIFI_SSID, CONFIG_KOUKI_WIFI_TIMEOUT_S);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool kouki_wifi_is_connected(void)
{
    return s_connected;
}
