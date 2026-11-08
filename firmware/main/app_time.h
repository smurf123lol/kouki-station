#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KOUKI_TIME_DEFAULT_TZ "MSK-3"

/**
 * @brief Инициализирует SNTP клиент и устанавливает часовой пояс.
 * 
 * Если tz_str == NULL или пустой, пытается прочитать ключ "timezone" из NVS,
 * иначе использует KOUKI_TIME_DEFAULT_TZ ("MSK-3").
 *
 * @param tz_str Строка таймзоны (POSIX TZ) или NULL.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t kouki_time_init(const char *tz_str);

/**
 * @brief Проверяет, синхронизировано ли время по SNTP.
 */
bool kouki_time_is_synced(void);

/**
 * @brief Возвращает текущее локальное время.
 * 
 * @param timeinfo Указатель на структуру struct tm для сохранения результата.
 * @return esp_err_t ESP_OK если время валидно, ESP_ERR_INVALID_STATE если не синхронизировано.
 */
esp_err_t kouki_time_get(struct tm *timeinfo);

/**
 * @brief Форматирует время в виде "HH:MM" (например, "18:05").
 * Если время не синхронизировано, записывает "--:--".
 */
void kouki_time_format_hm(char *buf, size_t max_len);

/**
 * @brief Форматирует дату в виде "вт, 8 ноя".
 * Если время не синхронизировано, записывает "--".
 */
void kouki_time_format_date(char *buf, size_t max_len);

/**
 * @brief Устанавливает адрес fallback SNTP сервера (например, IP шлюза роутера).
 */
void kouki_time_set_fallback_server(const char *server);

#ifdef __cplusplus
}
#endif

#endif // APP_TIME_H
