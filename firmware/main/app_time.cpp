#include "app_time.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef ESP_PLATFORM
#include "app_config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <sys/time.h>
#endif

namespace kouki {
namespace {

#ifdef ESP_PLATFORM
const char *const TAG = "kouki.time";
#endif

const char *const s_weekdays[] = {"вс", "пн", "вт", "ср", "чт", "пт", "сб"};
const char *const s_months[] = {"янв", "фев", "мар", "апр", "мая", "июн",
                                "июл", "авг", "сен", "окт", "ноя", "дек"};

constexpr long long kEpochThreshold = 1700000000LL;

}  // namespace

#ifdef ESP_PLATFORM
void Time::sntp_sync_notification_cb(struct timeval *tv)
{
    // Колбэк ставит сама SNTP-задача: экземпляр один, поэтому берём его
    // через фабрику, а не через user_data (API esp_sntp его не передаёт).
    timekeeper().on_sntp_sync(tv);
}

void Time::on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    m_synced.store(true, std::memory_order_release);
    time_t now = 0;
    struct tm timeinfo = {};
    ::time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "SNTP синхронизирован: %02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}
#endif

void Time::set_fallback_server(const char *server)
{
    if (!server) {
        m_fallback_server[0] = '\0';
        return;
    }
    strncpy(m_fallback_server, server, sizeof(m_fallback_server) - 1);
    m_fallback_server[sizeof(m_fallback_server) - 1] = '\0';
}

esp_err_t Time::init(const char *tz_str)
{
    const char *chosen_tz = tz_str;

#ifdef ESP_PLATFORM
    char tz_buffer[64] = {0};
    if (!chosen_tz || chosen_tz[0] == '\0') {
        nvs_handle_t handle;
        if (nvs_open(KOUKI_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            size_t len = sizeof(tz_buffer);
            if (nvs_get_str(handle, "timezone", tz_buffer, &len) == ESP_OK && tz_buffer[0] != '\0') {
                chosen_tz = tz_buffer;
            }
            nvs_close(handle);
        }
    }
#endif

    if (!chosen_tz || chosen_tz[0] == '\0') {
        chosen_tz = KOUKI_TIME_DEFAULT_TZ;
    }

    setenv("TZ", chosen_tz, 1);
    tzset();

#ifdef ESP_PLATFORM
    if (!m_initialized) {
        ESP_LOGI(TAG, "Инициализация SNTP (таймзона: %s)...", chosen_tz);
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        if (m_fallback_server[0] != '\0') {
            esp_sntp_setservername(1, m_fallback_server);
            ESP_LOGI(TAG, "Fallback SNTP сервер: %s", m_fallback_server);
        }
        esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
        esp_sntp_set_time_sync_notification_cb(&Time::sntp_sync_notification_cb);
        esp_sntp_init();
        m_initialized = true;
    }
#else
    m_initialized = true;
#endif

    return ESP_OK;
}

bool Time::is_synced()
{
    if (m_synced.load(std::memory_order_acquire)) {
        return true;
    }
#ifdef ESP_PLATFORM
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        m_synced.store(true, std::memory_order_release);
        return true;
    }
#endif
    time_t now = 0;
    ::time(&now);
    if ((long long)now > kEpochThreshold) {
        m_synced.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

esp_err_t Time::get(struct tm *timeinfo)
{
    if (!timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_synced()) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = 0;
    ::time(&now);
    localtime_r(&now, timeinfo);
    return ESP_OK;
}

void Time::format_hm(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) return;
    struct tm ti;
    memset(&ti, 0, sizeof(ti));
    if (get(&ti) != ESP_OK) {
        snprintf(buf, max_len, "--:--");
        return;
    }
    snprintf(buf, max_len, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

void Time::format_date(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) return;
    struct tm ti;
    memset(&ti, 0, sizeof(ti));
    if (get(&ti) != ESP_OK) {
        snprintf(buf, max_len, "--");
        return;
    }
    int wday = ti.tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    int mon = ti.tm_mon;
    if (mon < 0 || mon > 11) mon = 0;
    snprintf(buf, max_len, "%s, %d %s", s_weekdays[wday], ti.tm_mday, s_months[mon]);
}

Time &timekeeper()
{
    static Time s_instance;
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

void kouki_time_set_fallback_server(const char *server)
{
    kouki::timekeeper().set_fallback_server(server);
}

esp_err_t kouki_time_init(const char *tz_str)
{
    return kouki::timekeeper().init(tz_str);
}

bool kouki_time_is_synced(void)
{
    return kouki::timekeeper().is_synced();
}

esp_err_t kouki_time_get(struct tm *timeinfo)
{
    return kouki::timekeeper().get(timeinfo);
}

void kouki_time_format_hm(char *buf, size_t max_len)
{
    kouki::timekeeper().format_hm(buf, max_len);
}

void kouki_time_format_date(char *buf, size_t max_len)
{
    kouki::timekeeper().format_date(buf, max_len);
}

}  // extern "C"
