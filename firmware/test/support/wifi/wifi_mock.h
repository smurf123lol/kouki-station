#ifndef WIFI_MOCK_H
#define WIFI_MOCK_H

/*
 * Мок Wi-Fi/сети для host-стенда Wifi (T121 П7).
 *
 * Подменяет esp_wifi_*, esp_netif_*, esp_event_* и esp_timer_* так, чтобы
 * app_wifi.cpp собирался и работал без IDF. События «доставляет» либо сам мок
 * (esp_wifi_start -> STA_START, esp_wifi_connect -> GOT_IP при заданном
 * авто-адресе), либо тест через wifi_mock_fire_event().
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Очистка журналов, обработчиков и заготовленных ошибок. */
void wifi_mock_reset(void);

/*
 * Поведение шины:
 * - fire_sta_start (по умолчанию true): esp_wifi_start() доставляет
 *   WIFI_EVENT_STA_START, как IDF;
 * - auto_got_ip (по умолчанию выключен): esp_wifi_connect() доставляет
 *   IP_EVENT_STA_GOT_IP с заданным адресом и шлюзом.
 */
void wifi_mock_set_fire_sta_start(bool enabled);
void wifi_mock_set_auto_got_ip(bool enabled, uint32_t ip_addr, uint32_t gw_addr);

/** Заготовленные ошибки: следующий вызов вернёт заданный код (однократно). */
void wifi_mock_fail_next_init(esp_err_t err);
void wifi_mock_fail_next_timer_create(esp_err_t err);
void wifi_mock_fail_next_inactive_time(esp_err_t err);

/** Журнал вызовов. */
int wifi_mock_init_calls(void);
int wifi_mock_connect_calls(void);
int wifi_mock_start_calls(void);
int wifi_mock_set_mode_calls(void);
int wifi_mock_set_config_calls(void);
int wifi_mock_netif_init_calls(void);
int wifi_mock_create_sta_calls(void);
int wifi_mock_get_ip_info_calls(void);

/** Параметры последнего esp_wifi_set_config(). */
const char *wifi_mock_ssid(void);
const char *wifi_mock_password(void);
wifi_auth_mode_t wifi_mock_authmode(void);
/** Последний интерфейс/режим из set_config и set_mode. */
wifi_interface_t wifi_mock_config_interface(void);
wifi_mode_t wifi_mock_mode(void);
/** Последний inactive_time, секунды (-1, если не задавали). */
int wifi_mock_inactive_time(void);

/** Журнал таймера повтора. */
int wifi_mock_timer_create_calls(void);
int wifi_mock_timer_start_calls(void);
int wifi_mock_timer_stop_calls(void);
uint64_t wifi_mock_last_timer_us(void);
/** Имя таймера и arg, с которыми его создавали. */
const char *wifi_mock_timer_name(void);
void *wifi_mock_timer_arg(void);

/** Сколько обработчиков зарегистрировано (WIFI_EVENT + IP_EVENT). */
int wifi_mock_handler_count(void);

/** Доставить событие зарегистрированным обработчикам (имитация IDF). */
void wifi_mock_fire_event(esp_event_base_t base, int32_t id, void *data);
/** Дёрнуть колбэк таймера повтора, как это сделал бы esp_timer. */
void wifi_mock_fire_retry_timer(void);

/** Отдаваемый esp_netif_get_ip_info() адрес и RSSI точки доступа. */
void wifi_mock_set_ip(uint32_t ip_addr);
void wifi_mock_set_ap(bool ok, int8_t rssi);

/** Сколько раз создавали группу событий. */
int wifi_mock_event_group_create_calls(void);

#ifdef __cplusplus
}
#endif

#endif  // WIFI_MOCK_H
