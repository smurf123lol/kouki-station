#ifndef APP_WIFI_HPP
#define APP_WIFI_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: подписи kouki_wifi_* остаются общими для
// C- и C++-модулей.
#include "app_wifi.h"

// FreeRTOS.h обязан идти раньше event_groups.h/esp_timer.h — то же требование,
// что в IDF-заголовках.
#include "freertos/FreeRTOS.h"

#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"

namespace kouki {

class Config;

/**
 * Wi-Fi в режиме станции (T121 П7).
 *
 * Владеет состоянием модуля: группой событий подключения, атомарными
 * признаком связи и счётчиком изменений состояния, а также счётчиками и
 * таймером повторов. SSID и пароль берёт из Config (ссылка в конструкторе),
 * остальные настройки — из Kconfig.
 *
 * Колбэки esp_event остаются статическими функциями без внешнего состояния:
 * они получают указатель на экземпляр в user_arg (спека §5) и зовут метод.
 * Экземпляр один (см. kouki::wifi()), создаётся статически; деструктора нет —
 * модуль живёт до перезагрузки.
 */
class Wifi {
public:
    explicit Wifi(Config &config);
    Wifi(const Wifi &) = delete;
    Wifi &operator=(const Wifi &) = delete;

    /**
     * Поднимает esp_netif/esp_event/esp_wifi и блокируется до получения
     * адреса или до таймаута CONFIG_KOUKI_WIFI_TIMEOUT_S.
     */
    esp_err_t connect();

    /** Есть ли сейчас связь (обновляет колбэк GOT_IP/DISCONNECTED). */
    bool is_connected() const;

    /** Монотонный счётчик изменений состояния подключения/IP. */
    uint32_t state_seq() const;

    /** IP-адрес станции и RSSI (dBm); ESP_ERR_INVALID_STATE без связи. */
    esp_err_t get_info(char *ip_buf, size_t ip_len, int8_t *rssi);

private:
    /** Колбэк одноразового таймера повтора: снова идём в esp_wifi_connect(). */
    static void retry_timer_trampoline(void *arg);
    void on_retry_timer();

    /** Колбэк esp_event: один на WIFI_EVENT и IP_EVENT (различает base/id). */
    static void wifi_event_trampoline(void *arg, esp_event_base_t base, int32_t id, void *data);
    void on_wifi_event(esp_event_base_t base, int32_t id, void *data);

    /** Внутренние шаги connect(): сетевой стек, обработчики, старт станции. */
    esp_err_t bring_up_stack();
    esp_err_t configure_station();

    Config &m_config;

    EventGroupHandle_t m_events = nullptr;
    esp_timer_handle_t m_retry_timer = nullptr;

    std::atomic<bool> m_connected;
    std::atomic<uint32_t> m_state_seq;
    // Пауза и счётчик повторов трогает только колбэк событий (задача
    // esp_event), поэтому атомарность им не нужна — как и в прежнем модуле.
    uint32_t m_retry_delay_ms;
    uint32_t m_retry_count;
};

/** Единственный экземпляр Wi-Fi станции. */
Wifi &wifi();

}  // namespace kouki

#endif  // APP_WIFI_HPP
