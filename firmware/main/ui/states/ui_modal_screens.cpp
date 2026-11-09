#include "ui_modal_screens.hpp"
#include <cstdio>
#include <cstring>

namespace kouki::ui {

UiModalScreens::UiModalScreens(lv_obj_t* parent)
    : m_parent(parent)
{
}

UiModalScreens::~UiModalScreens()
{
    if (m_setup_overlay != nullptr) {
        lv_obj_del(m_setup_overlay);
        m_setup_overlay = nullptr;
    }
    if (m_ota_overlay != nullptr) {
        lv_obj_del(m_ota_overlay);
        m_ota_overlay = nullptr;
    }
}

void UiModalScreens::show_setup(const char* ssid, const char* url)
{
    if (m_setup_overlay == nullptr) {
        m_setup_overlay = lv_obj_create(m_parent);
        lv_obj_set_size(m_setup_overlay, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_align(m_setup_overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(m_setup_overlay, lv_color_hex(COLOR_INK), 0);
        lv_obj_set_style_border_width(m_setup_overlay, 0, 0);
        lv_obj_clear_flag(m_setup_overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title = lv_label_create(m_setup_overlay);
        lv_label_set_text(title, "Настройка станции");
        lv_obj_set_style_text_font(title, &font_ru_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_WARN), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

        m_setup_text = lv_label_create(m_setup_overlay);
        lv_label_set_long_mode(m_setup_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m_setup_text, 280);
        lv_obj_set_style_text_font(m_setup_text, &font_ru_16, 0);
        lv_obj_set_style_text_color(m_setup_text, lv_color_hex(COLOR_SAND), 0);
        lv_obj_set_style_text_align(m_setup_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(m_setup_text, LV_ALIGN_CENTER, 0, 20);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Подключитесь к Wi-Fi:\n%s\n\nОткройте в браузере:\n%s",
             ssid != nullptr ? ssid : "Kouki-Setup",
             url != nullptr ? url : "192.168.4.1");
    lv_label_set_text(m_setup_text, buf);
    lv_obj_clear_flag(m_setup_overlay, LV_OBJ_FLAG_HIDDEN);
    m_setup_active.store(true, std::memory_order_release);
}

void UiModalScreens::clear_setup()
{
    m_setup_active.store(false, std::memory_order_release);
    if (m_setup_overlay != nullptr) {
        lv_obj_add_flag(m_setup_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void UiModalScreens::show_ota(const char* text)
{
    if (m_ota_overlay == nullptr) {
        m_ota_overlay = lv_obj_create(m_parent);
        lv_obj_set_size(m_ota_overlay, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_align(m_ota_overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(m_ota_overlay, lv_color_hex(COLOR_INK), 0);
        lv_obj_set_style_border_width(m_ota_overlay, 0, 0);
        lv_obj_clear_flag(m_ota_overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title = lv_label_create(m_ota_overlay);
        lv_label_set_text(title, "Обновление ПО");
        lv_obj_set_style_text_font(title, &font_ru_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_WARN), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

        m_ota_spinner = lv_arc_create(m_ota_overlay);
        lv_obj_set_size(m_ota_spinner, 80, 80);
        lv_obj_align(m_ota_spinner, LV_ALIGN_CENTER, 0, -10);
        lv_obj_remove_style(m_ota_spinner, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(m_ota_spinner, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(m_ota_spinner, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(m_ota_spinner, 6, LV_PART_INDICATOR);
        lv_arc_set_bg_angles(m_ota_spinner, 0, 360);
        lv_arc_set_angles(m_ota_spinner, 0, 100);

        m_ota_text = lv_label_create(m_ota_overlay);
        lv_label_set_long_mode(m_ota_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m_ota_text, 280);
        lv_obj_set_style_text_font(m_ota_text, &font_ru_16, 0);
        lv_obj_set_style_text_color(m_ota_text, lv_color_hex(COLOR_SAND), 0);
        lv_obj_set_style_text_align(m_ota_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(m_ota_text, LV_ALIGN_BOTTOM_MID, 0, -40);
    }

    if (text != nullptr) {
        lv_label_set_text(m_ota_text, text);
    }
    lv_obj_clear_flag(m_ota_overlay, LV_OBJ_FLAG_HIDDEN);
    m_ota_active.store(true, std::memory_order_release);
}

void UiModalScreens::clear_ota()
{
    m_ota_active.store(false, std::memory_order_release);
    if (m_ota_overlay != nullptr) {
        lv_obj_add_flag(m_ota_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace kouki::ui
