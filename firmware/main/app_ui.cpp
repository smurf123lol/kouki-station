#include "app_ui.h"
#include "ui/ui_state_manager.hpp"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "kouki.ui";

static void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    kouki::ui::UiStateManager::instance().step(33); // 33 мс = 30 кадров/сек
}

extern "C" void kouki_ui_init(void)
{
    ESP_LOGI(TAG, "инициализация Kouki UI (C++ State Machine)");
    kouki::ui::UiStateManager::instance().init();
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
