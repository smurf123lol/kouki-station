#include "app_ui.h"
#include "ui/ui_state_manager.hpp"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "kouki.ui";
static int64_t s_last_tick_us = 0;

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = 33;
    if (s_last_tick_us > 0 && now_us > s_last_tick_us) {
        int64_t delta_ms = (now_us - s_last_tick_us) / 1000;
        if (delta_ms < 1) {
            delta_ms = 1;
        } else if (delta_ms > 2000) {
            delta_ms = 2000;
        }
        dt_ms = static_cast<uint32_t>(delta_ms);
    }
    s_last_tick_us = now_us;
    kouki::ui::UiStateManager::instance().step(dt_ms);
}

extern "C" void kouki_ui_init(void)
{
    ESP_LOGI(TAG, "инициализация Kouki UI (C++ State Machine)");
    kouki::ui::UiStateManager::instance().init();
    s_last_tick_us = esp_timer_get_time();
    lv_timer_create(ui_tick_cb, 33, nullptr);
}

extern "C" void kouki_ui_show_setup(const char *ssid, const char *url)
{
    kouki::ui::UiStateManager::instance().show_setup(ssid, url);
}

extern "C" void kouki_ui_show_ota_progress(const char *text)
{
    kouki::ui::UiStateManager::instance().show_ota_progress(text);
}

extern "C" void kouki_ui_clear_ota(void)
{
    kouki::ui::UiStateManager::instance().clear_ota();
}
