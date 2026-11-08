#include "app_time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "app_config.h"

static const char *TAG = "kouki.time";
#endif

static const char *s_weekdays[] = { "вс", "пн", "вт", "ср", "чт", "пт", "сб" };
static const char *s_months[] = { "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек" };

static bool s_time_initialized = false;
static bool s_time_synced = false;
static char s_fallback_server[64] = {0};

#ifdef ESP_PLATFORM
static void sntp_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "SNTP синхронизирован: %02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}
#endif

void kouki_time_set_fallback_server(const char *server)
{
    if (!server) {
        s_fallback_server[0] = '\0';
        return;
    }
    strncpy(s_fallback_server, server, sizeof(s_fallback_server) - 1);
    s_fallback_server[sizeof(s_fallback_server) - 1] = '\0';
}

esp_err_t kouki_time_init(const char *tz_str)
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
    if (!s_time_initialized) {
        ESP_LOGI(TAG, "Инициализация SNTP (таймзона: %s)...", chosen_tz);
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        if (s_fallback_server[0] != '\0') {
            esp_sntp_setservername(1, s_fallback_server);
            ESP_LOGI(TAG, "Fallback SNTP сервер: %s", s_fallback_server);
        }
        esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
        esp_sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);
        esp_sntp_init();
        s_time_initialized = true;
    }
#else
    s_time_initialized = true;
#endif

    return ESP_OK;
}

bool kouki_time_is_synced(void)
{
    if (s_time_synced) {
        return true;
    }
#ifdef ESP_PLATFORM
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_time_synced = true;
        return true;
    }
#endif
    time_t now = 0;
    time(&now);
    // 1700000000 = ~Nov 2023 (epoch validation threshold)
    if (now > 1700000000) {
        s_time_synced = true;
        return true;
    }
    return false;
}

esp_err_t kouki_time_get(struct tm *timeinfo)
{
    if (!timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kouki_time_is_synced()) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t now = 0;
    time(&now);
    localtime_r(&now, timeinfo);
    return ESP_OK;
}

void kouki_time_format_hm(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) return;
    struct tm ti;
    memset(&ti, 0, sizeof(ti));
    if (kouki_time_get(&ti) != ESP_OK) {
        snprintf(buf, max_len, "--:--");
        return;
    }
    snprintf(buf, max_len, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

void kouki_time_format_date(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) return;
    struct tm ti;
    memset(&ti, 0, sizeof(ti));
    if (kouki_time_get(&ti) != ESP_OK) {
        snprintf(buf, max_len, "--");
        return;
    }
    int wday = ti.tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    int mon = ti.tm_mon;
    if (mon < 0 || mon > 11) mon = 0;
    snprintf(buf, max_len, "%s, %d %s", s_weekdays[wday], ti.tm_mday, s_months[mon]);
}
