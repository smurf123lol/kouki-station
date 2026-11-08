#include "ui_settings_tile.hpp"
#include "ui_state_manager.hpp"
#include "app_config.h"
#include "app_wifi.h"
#include "app_ota.h"
#include "esp_board_init.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "kouki.ui.settings";

namespace kouki::ui {

static void on_settings_interact(lv_event_t* e)
{
    auto* tile = static_cast<SettingsTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
}

static void on_volume_changed(lv_event_t* e)
{
    auto* tile = static_cast<SettingsTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
    lv_obj_t* slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    esp_audio_set_play_vol(val);
    if (tile != nullptr) {
        tile->update_vol_label(val);
    }
    ESP_LOGI(TAG, "громкость изменена: %ld%%", (long)val);
}

static void on_wake_toggle(lv_event_t* e)
{
    auto* tile = static_cast<SettingsTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
    lv_obj_t* sw = lv_event_get_target(e);
    bool en = lv_obj_has_state(sw, LV_STATE_CHECKED);
    kouki_config_set_wake_enabled(en);
    kouki_config_save();
    ESP_LOGI(TAG, "wake-word переключен: %s", en ? "вкл" : "выкл");
}

static void on_wake_sens_changed(lv_event_t* e)
{
    auto* tile = static_cast<SettingsTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
    lv_obj_t* slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    kouki_config_set_wake_threshold(static_cast<uint8_t>(val));
    kouki_config_save();
    if (tile != nullptr) {
        tile->update_wake_sens_label(val);
    }
    ESP_LOGI(TAG, "чувствительность wake-word изменена: %ld%%", (long)val);
}

static void on_silent_toggle(lv_event_t* e)
{
    auto* tile = static_cast<SettingsTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
    lv_obj_t* sw = lv_event_get_target(e);
    bool silent = lv_obj_has_state(sw, LV_STATE_CHECKED);
    kouki_config_set_silent(silent);
    kouki_config_save();
    ESP_LOGI(TAG, "режим тишины: %s", silent ? "вкл" : "выкл");
}

static void on_ota_btn_clicked(lv_event_t* e)
{
    auto* tile = static_cast<SettingsTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
    ESP_LOGI(TAG, "запрос проверки OTA");
    kouki_ota_check_now();
}

SettingsTile::SettingsTile(lv_obj_t* parent, UiStateManager* manager)
    : m_manager(manager)
{
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(m_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(m_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_pad_all(m_container, 0, 0);
    lv_obj_clear_flag(m_container, LV_OBJ_FLAG_SCROLLABLE);

    // Заголовок "Настройки"
    lv_obj_t* title = lv_label_create(m_container);
    lv_label_set_text(title, "Настройки");
    lv_obj_set_style_text_font(title, &font_ru_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // Вертикальный контейнер скролла
    m_scroll_container = lv_obj_create(m_container);
    lv_obj_set_size(m_scroll_container, 310, 290);
    lv_obj_align(m_scroll_container, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_opa(m_scroll_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_scroll_container, 0, 0);
    lv_obj_set_style_pad_hor(m_scroll_container, 15, 0);
    lv_obj_set_style_pad_ver(m_scroll_container, 8, 0);
    lv_obj_set_flex_flow(m_scroll_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m_scroll_container, 12, 0);
    lv_obj_set_scroll_dir(m_scroll_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(m_scroll_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(m_scroll_container, on_settings_interact, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(m_scroll_container, on_settings_interact, LV_EVENT_SCROLL, this);

    // 1. Секция: Громкость
    int current_vol = 70;
    esp_audio_get_play_vol(&current_vol);

    m_vol_label = lv_label_create(m_scroll_container);
    char vol_buf[32];
    snprintf(vol_buf, sizeof(vol_buf), "Громкость: %d%%", current_vol);
    lv_label_set_text(m_vol_label, vol_buf);
    lv_obj_set_style_text_font(m_vol_label, &font_ru_14, 0);
    lv_obj_set_style_text_color(m_vol_label, lv_color_hex(COLOR_SAND), 0);

    m_vol_slider = lv_slider_create(m_scroll_container);
    lv_obj_set_width(m_vol_slider, 270);
    lv_slider_set_range(m_vol_slider, 0, 100);
    lv_slider_set_value(m_vol_slider, current_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(m_vol_slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(m_vol_slider, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(m_vol_slider, on_volume_changed, LV_EVENT_VALUE_CHANGED, this);

    // 2. Секция: Wake-word тумблер
    lv_obj_t* wake_row = lv_obj_create(m_scroll_container);
    lv_obj_set_size(wake_row, 270, 40);
    lv_obj_set_style_bg_opa(wake_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wake_row, 0, 0);
    lv_obj_set_style_pad_all(wake_row, 0, 0);
    lv_obj_clear_flag(wake_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* wake_lbl = lv_label_create(wake_row);
    lv_label_set_text(wake_lbl, "Активация «эй гин»");
    lv_obj_set_style_text_font(wake_lbl, &font_ru_14, 0);
    lv_obj_set_style_text_color(wake_lbl, lv_color_hex(COLOR_SAND), 0);
    lv_obj_align(wake_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    m_wake_sw = lv_switch_create(wake_row);
    lv_obj_align(m_wake_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (kouki_config_get_wake_enabled()) {
        lv_obj_add_state(m_wake_sw, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(m_wake_sw, lv_color_hex(COLOR_ACCENT), static_cast<uint32_t>(LV_PART_INDICATOR) | static_cast<uint32_t>(LV_STATE_CHECKED));
    lv_obj_add_event_cb(m_wake_sw, on_wake_toggle, LV_EVENT_VALUE_CHANGED, this);

    // Слайдер чувствительности wake-word
    uint8_t current_sens = kouki_config_get_wake_threshold();
    m_wake_sens_label = lv_label_create(m_scroll_container);
    char sens_buf[40];
    snprintf(sens_buf, sizeof(sens_buf), "Чувствительность: %u%%", (unsigned)current_sens);
    lv_label_set_text(m_wake_sens_label, sens_buf);
    lv_obj_set_style_text_font(m_wake_sens_label, &font_ru_14, 0);
    lv_obj_set_style_text_color(m_wake_sens_label, lv_color_hex(COLOR_DIM), 0);

    m_wake_slider = lv_slider_create(m_scroll_container);
    lv_obj_set_width(m_wake_slider, 270);
    lv_slider_set_range(m_wake_slider, 10, 95);
    lv_slider_set_value(m_wake_slider, current_sens, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(m_wake_slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(m_wake_slider, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(m_wake_slider, on_wake_sens_changed, LV_EVENT_VALUE_CHANGED, this);

    // 3. Секция: Режим тишины
    lv_obj_t* silent_row = lv_obj_create(m_scroll_container);
    lv_obj_set_size(silent_row, 270, 40);
    lv_obj_set_style_bg_opa(silent_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(silent_row, 0, 0);
    lv_obj_set_style_pad_all(silent_row, 0, 0);
    lv_obj_clear_flag(silent_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* silent_lbl = lv_label_create(silent_row);
    lv_label_set_text(silent_lbl, "Режим без звука");
    lv_obj_set_style_text_font(silent_lbl, &font_ru_14, 0);
    lv_obj_set_style_text_color(silent_lbl, lv_color_hex(COLOR_SAND), 0);
    lv_obj_align(silent_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    m_silent_sw = lv_switch_create(silent_row);
    lv_obj_align(m_silent_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (kouki_config_is_silent()) {
        lv_obj_add_state(m_silent_sw, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(m_silent_sw, lv_color_hex(COLOR_ACCENT), static_cast<uint32_t>(LV_PART_INDICATOR) | static_cast<uint32_t>(LV_STATE_CHECKED));
    lv_obj_add_event_cb(m_silent_sw, on_silent_toggle, LV_EVENT_VALUE_CHANGED, this);

    // 4. Секция: Wi-Fi карточка
    lv_obj_t* wifi_card = lv_obj_create(m_scroll_container);
    lv_obj_set_width(wifi_card, 270);
    lv_obj_set_height(wifi_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(wifi_card, lv_color_hex(COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(wifi_card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(wifi_card, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_border_width(wifi_card, 1, 0);
    lv_obj_set_style_radius(wifi_card, 8, 0);
    lv_obj_set_style_pad_all(wifi_card, 8, 0);
    lv_obj_clear_flag(wifi_card, LV_OBJ_FLAG_SCROLLABLE);

    m_wifi_label = lv_label_create(wifi_card);
    lv_label_set_long_mode(m_wifi_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m_wifi_label, 250);
    lv_obj_set_style_text_font(m_wifi_label, &font_ru_14, 0);
    lv_obj_set_style_text_color(m_wifi_label, lv_color_hex(COLOR_SAND), 0);
    refresh_wifi();

    // 5. Секция: OTA кнопка и версия
    const esp_app_desc_t* desc = esp_app_get_description();
    const char* ver_str = (desc != nullptr) ? desc->version : "0.1.5";

    char ver_buf[80];
    snprintf(ver_buf, sizeof(ver_buf), "Версия ПО: %s", ver_str);
    lv_obj_t* ver_lbl = lv_label_create(m_scroll_container);
    lv_label_set_text(ver_lbl, ver_buf);
    lv_obj_set_style_text_font(ver_lbl, &font_ru_14, 0);
    lv_obj_set_style_text_color(ver_lbl, lv_color_hex(COLOR_DIM), 0);

    lv_obj_t* ota_btn = lv_btn_create(m_scroll_container);
    lv_obj_set_size(ota_btn, 270, 42);
    lv_obj_set_style_bg_color(ota_btn, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_radius(ota_btn, 8, 0);
    lv_obj_add_event_cb(ota_btn, on_ota_btn_clicked, LV_EVENT_CLICKED, this);

    m_ota_btn_label = lv_label_create(ota_btn);
    lv_label_set_text(m_ota_btn_label, "Проверить обновления");
    lv_obj_set_style_text_font(m_ota_btn_label, &font_ru_16, 0);
    lv_obj_set_style_text_color(m_ota_btn_label, lv_color_hex(COLOR_INK), 0);
    lv_obj_center(m_ota_btn_label);
}

SettingsTile::~SettingsTile()
{
    if (m_container != nullptr) {
        lv_obj_del(m_container);
        m_container = nullptr;
    }
}

void SettingsTile::update_vol_label(int val)
{
    if (m_vol_label != nullptr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Громкость: %d%%", val);
        lv_label_set_text(m_vol_label, buf);
    }
}

void SettingsTile::update_wake_sens_label(int val)
{
    if (m_wake_sens_label != nullptr) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Чувствительность: %d%%", val);
        lv_label_set_text(m_wake_sens_label, buf);
    }
}

void SettingsTile::refresh_wifi()
{
    char ip[24] = {0};
    int8_t rssi = 0;
    const char* ssid = kouki_config_get_wifi_ssid();

    if (kouki_wifi_is_connected()) {
        kouki_wifi_get_info(ip, sizeof(ip), &rssi);
        char text[128];
        snprintf(text, sizeof(text), "Wi-Fi: %s\nIP: %s (%d dBm)",
                 ssid != nullptr ? ssid : "-",
                 ip[0] != '\0' ? ip : "-",
                 (int)rssi);
        lv_label_set_text(m_wifi_label, text);
    } else {
        char text[96];
        snprintf(text, sizeof(text), "Wi-Fi: %s\n(нет подключения)", ssid != nullptr ? ssid : "-");
        lv_label_set_text(m_wifi_label, text);
    }
}

void SettingsTile::reset_inactivity()
{
    m_inactivity_ms = 0;
}

void SettingsTile::step(uint32_t dt_ms)
{
    m_wifi_poll_ms += dt_ms;
    if (m_wifi_poll_ms >= 5000) {
        m_wifi_poll_ms = 0;
        refresh_wifi();
    }

    // Автовозврат на главный экран через 20 секунд неактивности
    if (m_manager != nullptr && m_manager->get_active_tile() == TileId::TILE_SETTINGS) {
        m_inactivity_ms += dt_ms;
        if (m_inactivity_ms >= 20000) {
            m_inactivity_ms = 0;
            m_manager->focus_voice_tile(true);
        }
    } else {
        m_inactivity_ms = 0;
    }
}

} // namespace kouki::ui
