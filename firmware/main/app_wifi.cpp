#include "app_wifi.hpp"

#include <cstring>

#include "app_config.hpp"
#include "app_time.hpp"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

namespace kouki {

namespace {

const char *const TAG = "kouki.wifi";

/* Биты группы событий: прежние CONNECTED_BIT/FAILED_BIT. */
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

/* Повторы с растущей паузой: при выключенном роутере плата не крутит цикл
 * подключений, а ждёт 1, 2, 4, 8, 16 секунд (дальше — каждые 16). Счётчик
 * сбрасывается, когда адрес получен. Пауза — одноразовый таймер, а не
 * vTaskDelay: обработчик события должен оставаться быстрым. */
constexpr uint32_t kRetryMaxMs = 16000;
constexpr uint32_t kRetryStartMs = 1000;

}  // namespace

Wifi::Wifi(Config &config)
    : m_config(config),
      m_connected(false),
      m_state_seq(0),
      m_retry_delay_ms(kRetryStartMs),
      m_retry_count(0)
{
}

void Wifi::retry_timer_trampoline(void *arg)
{
    // Экземпляр один, но указатель всё равно приходит из esp_timer_create_args_t.
    if (arg != nullptr) {
        static_cast<Wifi *>(arg)->on_retry_timer();
    }
}

void Wifi::on_retry_timer()
{
    esp_wifi_connect();
}

void Wifi::wifi_event_trampoline(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (arg != nullptr) {
        static_cast<Wifi *>(arg)->on_wifi_event(base, id, data);
    }
}

void Wifi::on_wifi_event(esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        m_connected.store(false, std::memory_order_release);
        m_state_seq.fetch_add(1, std::memory_order_release);
        m_retry_count++;
        const uint32_t delay_ms = m_retry_delay_ms;
        m_retry_delay_ms = delay_ms >= kRetryMaxMs ? kRetryMaxMs : delay_ms * 2;
        ESP_LOGW(TAG, "соединение потеряно (попытка %u), повтор через %u мс",
                 (unsigned)m_retry_count, (unsigned)delay_ms);
        if (m_retry_timer != nullptr) {
            esp_timer_stop(m_retry_timer);
            esp_timer_start_once(m_retry_timer, (uint64_t)delay_ms * 1000);
        } else {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "адрес получен: " IPSTR, IP2STR(&event->ip_info.ip));
        if (event->ip_info.gw.addr != 0) {
            char gw_str[16] = {0};
            esp_ip4addr_ntoa(&event->ip_info.gw, gw_str, sizeof(gw_str));
            timekeeper().set_fallback_server(gw_str);
        }
        timekeeper().init(nullptr);
        m_connected.store(true, std::memory_order_release);
        m_state_seq.fetch_add(1, std::memory_order_release);
        m_retry_delay_ms = kRetryStartMs;
        m_retry_count = 0;
        if (m_retry_timer != nullptr) {
            esp_timer_stop(m_retry_timer);
        }
        xEventGroupSetBits(m_events, kConnectedBit);
    }
}

esp_err_t Wifi::bring_up_stack()
{
    m_events = xEventGroupCreate();
    if (!m_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &Wifi::retry_timer_trampoline;
    timer_args.name = "wifi-retry";
    timer_args.arg = this;
    const esp_err_t timer_err = esp_timer_create(&timer_args, &m_retry_timer);
    if (timer_err != ESP_OK) {
        ESP_LOGW(TAG, "таймер повторов не создан: %s (повторы без паузы)", esp_err_to_name(timer_err));
        m_retry_timer = nullptr;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Wifi::wifi_event_trampoline, this, nullptr),
        TAG, "регистрация WIFI_EVENT");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Wifi::wifi_event_trampoline, this, nullptr),
        TAG, "регистрация IP_EVENT");

    return ESP_OK;
}

esp_err_t Wifi::configure_station()
{
    if (!m_config.is_complete()) {
        ESP_LOGE(TAG, "конфигурация не готова: Wi-Fi не настроен");
        return ESP_ERR_INVALID_STATE;
    }

    const char *ssid = m_config.wifi_ssid();
    const char *pass = m_config.wifi_pass();
    if (ssid == nullptr || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pass != nullptr && strlen(pass) > 0) {
        strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    /* T95: определяем обрыв AP за 3 с (минимум в IDF для STA) вместо стандартных 6 с */
    const esp_err_t inactive_err = esp_wifi_set_inactive_time(WIFI_IF_STA, 3);
    if (inactive_err != ESP_OK) {
        ESP_LOGW(TAG, "не удалось установить inactive_time: %s", esp_err_to_name(inactive_err));
    }

    return ESP_OK;
}

esp_err_t Wifi::connect()
{
    const esp_err_t stack_err = bring_up_stack();
    if (stack_err != ESP_OK) {
        return stack_err;
    }

    const esp_err_t cfg_err = configure_station();
    if (cfg_err != ESP_OK) {
        return cfg_err;
    }

    ESP_LOGI(TAG, "подключаюсь к Wi-Fi");
    const EventBits_t bits = xEventGroupWaitBits(m_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(CONFIG_KOUKI_WIFI_TIMEOUT_S * 1000));
    if ((bits & kConnectedBit) == 0) {
        ESP_LOGE(TAG, "сеть недоступна за %d с", CONFIG_KOUKI_WIFI_TIMEOUT_S);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool Wifi::is_connected() const
{
    return m_connected.load(std::memory_order_acquire);
}

uint32_t Wifi::state_seq() const
{
    return m_state_seq.load(std::memory_order_acquire);
}

esp_err_t Wifi::get_info(char *ip_buf, size_t ip_len, int8_t *rssi)
{
    if (!is_connected()) {
        if (ip_buf != nullptr && ip_len > 0) {
            ip_buf[0] = '\0';
        }
        if (rssi != nullptr) {
            *rssi = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (ip_buf != nullptr && ip_len > 0) {
        esp_netif_ip_info_t ip_info = {};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_buf, ip_len);
        } else {
            ip_buf[0] = '\0';
        }
    }

    if (rssi != nullptr) {
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            *rssi = ap_info.rssi;
        } else {
            *rssi = 0;
        }
    }

    return ESP_OK;
}

Wifi &wifi()
{
    static Wifi s_instance(config());
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

esp_err_t kouki_wifi_connect(void)
{
    return kouki::wifi().connect();
}

bool kouki_wifi_is_connected(void)
{
    return kouki::wifi().is_connected();
}

uint32_t kouki_wifi_state_seq(void)
{
    return kouki::wifi().state_seq();
}

esp_err_t kouki_wifi_get_info(char *ip_buf, size_t ip_len, int8_t *rssi)
{
    return kouki::wifi().get_info(ip_buf, ip_len, rssi);
}

}  // extern "C"
