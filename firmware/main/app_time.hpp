#ifndef APP_TIME_HPP
#define APP_TIME_HPP

#include <atomic>
#include <cstddef>
#include <ctime>

// C-совместимый заголовок фасада: константы и типы kouki_time_* остаются
// общими для C- и C++-модулей.
#include "app_time.h"

struct timeval;

namespace kouki {

/**
 * Время и SNTP (T121 П3).
 *
 * Владеет состоянием модуля: признак инициализации, признак синхронизации
 * (его ставит колбэк SNTP из системной задачи) и адрес fallback-сервера.
 * Часовой пояс по-прежнему берётся из NVS ("timezone"), иначе
 * KOUKI_TIME_DEFAULT_TZ. Экземпляр один (см. kouki::timekeeper()), создаётся
 * статически; init() зовётся из app_main при старте Wi-Fi.
 */
class Time {
public:
    Time() = default;
    Time(const Time &) = delete;
    Time &operator=(const Time &) = delete;

    /** Задаёт fallback SNTP-сервер (NULL или пустая строка — сброс). */
    void set_fallback_server(const char *server);

    /** Настраивает TZ и (единожды) запускает SNTP. */
    esp_err_t init(const char *tz_str);

    /** Синхронизировано ли время (SNTP-статус либо эпоха выше порога). */
    bool is_synced();

    /** Текущее локальное время; ESP_ERR_INVALID_STATE, если не синхронизировано. */
    esp_err_t get(struct tm *timeinfo);

    /** "HH:MM" или "--:--", если время не синхронизировано. */
    void format_hm(char *buf, size_t max_len);

    /** "вт, 8 ноя" или "--", если время не синхронизировано. */
    void format_date(char *buf, size_t max_len);

private:
#ifdef ESP_PLATFORM
    static void sntp_sync_notification_cb(struct timeval *tv);
    void on_sntp_sync(struct timeval *tv);
#endif

    bool m_initialized = false;
    std::atomic<bool> m_synced{false};
    char m_fallback_server[64] = {0};
};

/**
 * Единственный экземпляр. Имя не time(), чтобы не перекрывать ::time()
 * внутри методов класса.
 */
Time &timekeeper();

}  // namespace kouki

#endif  // APP_TIME_HPP
