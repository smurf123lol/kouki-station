#include "ui_orb_widget.hpp"
#include <cstdio>
#include <cmath>

namespace kouki::ui {

OrbWidget::OrbWidget(lv_obj_t* parent)
{
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(m_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(m_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_pad_all(m_container, 0, 0);
    lv_obj_clear_flag(m_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_container, LV_OBJ_FLAG_CLICKABLE);

    // Wi-Fi точка вверху (разнесена с мини-сферой SubDone, чтобы не перекрываться)
    m_wifi_dot = lv_obj_create(m_container);
    lv_obj_set_size(m_wifi_dot, 8, 8);
    lv_obj_set_style_radius(m_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(m_wifi_dot, lv_color_hex(COLOR_DANGER), 0);
    lv_obj_set_style_border_width(m_wifi_dot, 0, 0);
    lv_obj_align(m_wifi_dot, LV_ALIGN_TOP_MID, 0, 11);
    lv_obj_clear_flag(m_wifi_dot, LV_OBJ_FLAG_CLICKABLE);

    // Главное кольцо сферы (диаметр 260px в нормальном состоянии)
    m_ring = lv_obj_create(m_container);
    lv_obj_set_size(m_ring, 260, 260);
    lv_obj_set_style_radius(m_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_border_width(m_ring, 3, 0);
    lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_CLICKABLE);

    // Внешняя дуга затухания тишины (VAD Arc, диаметр 276px)
    m_silence_arc = lv_arc_create(m_container);
    lv_obj_set_size(m_silence_arc, 276, 276);
    lv_obj_align(m_silence_arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_style(m_silence_arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(m_silence_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(m_silence_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_opa(m_silence_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(m_silence_arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(m_silence_arc, 4, LV_PART_INDICATOR);
    lv_arc_set_rotation(m_silence_arc, 270); // Старт сверху (12 часов)
    lv_arc_set_bg_angles(m_silence_arc, 0, 360);
    lv_arc_set_angles(m_silence_arc, 0, 360);
    lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);

    // Спиннер вращающейся дуги (Sending, диаметр 268px)
    m_spinner_arc = lv_arc_create(m_container);
    lv_obj_set_size(m_spinner_arc, 268, 268);
    lv_obj_align(m_spinner_arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_style(m_spinner_arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(m_spinner_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(m_spinner_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_opa(m_spinner_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(m_spinner_arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(m_spinner_arc, 4, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(m_spinner_arc, 0, 360);
    lv_arc_set_angles(m_spinner_arc, 0, 90);
    lv_obj_add_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

    // Часы крупным шрифтом (20 пт)
    m_time_label = lv_label_create(m_container);
    lv_label_set_text(m_time_label, "--:--");
    lv_obj_set_style_text_font(m_time_label, &font_ru_20, 0);
    lv_obj_set_style_text_color(m_time_label, lv_color_hex(COLOR_SAND), 0);
    lv_obj_align(m_time_label, LV_ALIGN_CENTER, 0, -14);
    lv_obj_clear_flag(m_time_label, LV_OBJ_FLAG_CLICKABLE);

    // Дата мелким шрифтом (14 пт)
    m_date_label = lv_label_create(m_container);
    lv_label_set_text(m_date_label, "--");
    lv_obj_set_style_text_font(m_date_label, &font_ru_14, 0);
    lv_obj_set_style_text_color(m_date_label, lv_color_hex(COLOR_DIM), 0);
    lv_obj_align(m_date_label, LV_ALIGN_CENTER, 0, 16);
    lv_obj_clear_flag(m_date_label, LV_OBJ_FLAG_CLICKABLE);

    // Статус состояния
    m_status_label = lv_label_create(m_container);
    lv_label_set_text(m_status_label, "");
    lv_label_set_long_mode(m_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m_status_label, 260);
    lv_obj_set_style_text_align(m_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(m_status_label, &font_ru_20, 0);
    lv_obj_align(m_status_label, LV_ALIGN_CENTER, 0, -12);
    lv_obj_add_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m_status_label, LV_OBJ_FLAG_CLICKABLE);

    // Дополнительный счетчик / длительность
    m_substatus_label = lv_label_create(m_container);
    lv_label_set_text(m_substatus_label, "");
    lv_obj_set_style_text_font(m_substatus_label, &font_ru_16, 0);
    lv_obj_set_style_text_color(m_substatus_label, lv_color_hex(COLOR_DIM), 0);
    lv_obj_align(m_substatus_label, LV_ALIGN_CENTER, 0, 16);
    lv_obj_add_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m_substatus_label, LV_OBJ_FLAG_CLICKABLE);
}

OrbWidget::~OrbWidget()
{
    if (m_container != nullptr) {
        lv_obj_del(m_container);
        m_container = nullptr;
    }
}

void OrbWidget::set_mode(UiVoiceSubStateId mode)
{
    m_mode = mode;
    m_last_dt_ms = 0;
    m_last_metric_silence_ms = 0;
    m_display_silence_ms = 0;
    m_last_total_sec = UINT32_MAX;
    m_last_arc_angle = 0;
    m_arc_visible = false;

    switch (mode) {
    case UiVoiceSubStateId::SUB_IDLE:
        lv_obj_set_size(m_ring, 260, 260);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_SAND), 0);
        lv_obj_set_style_border_width(m_ring, 3, 0);
        lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(m_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(m_date_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UiVoiceSubStateId::SUB_RECORDING:
        lv_obj_set_size(m_ring, 260, 260);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(m_ring, 4, 0);
        lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_date_label, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(m_status_label, "Слушаю…");
        lv_obj_set_style_text_color(m_status_label, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_clear_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(m_substatus_label, "00:00");
        lv_obj_clear_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UiVoiceSubStateId::SUB_SENDING:
        lv_obj_set_size(m_ring, 260, 260);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_PAPER), 0);
        lv_obj_set_style_border_width(m_ring, 2, 0);
        lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_date_label, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(m_status_label, "Думаю…");
        lv_obj_set_style_text_color(m_status_label, lv_color_hex(COLOR_DIM), 0);
        lv_obj_clear_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UiVoiceSubStateId::SUB_DONE:
        // Мини-сфера вверху экрана (центр y=42: ниже Wi-Fi точки y=11..19 и выше карточки y=65)
        lv_obj_set_size(m_ring, 32, 32);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, MINI_ORB_TAP_CENTER_Y_OFFSET);
        lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_SAND), 0);
        lv_obj_set_style_border_width(m_ring, 3, 0);
        lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_date_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UiVoiceSubStateId::SUB_FOLLOWUP:
        lv_obj_set_size(m_ring, 260, 260);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_WARN), 0);
        lv_obj_set_style_border_width(m_ring, 3, 0);
        lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_date_label, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(m_status_label, "Слушаю продолжение…");
        lv_obj_set_style_text_color(m_status_label, lv_color_hex(COLOR_WARN), 0);
        lv_obj_clear_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
        break;

    case UiVoiceSubStateId::SUB_ERROR:
        lv_obj_set_size(m_ring, 260, 260);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_color(m_ring, lv_color_hex(COLOR_DANGER), 0);
        lv_obj_set_style_border_width(m_ring, 4, 0);
        lv_obj_clear_flag(m_ring, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_spinner_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(m_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_date_label, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_text_color(m_status_label, lv_color_hex(COLOR_DANGER), 0);
        lv_obj_clear_flag(m_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_substatus_label, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

void OrbWidget::update_time(const char* hm_str, const char* date_str)
{
    if (hm_str != nullptr) {
        lv_label_set_text(m_time_label, hm_str);
    }
    if (date_str != nullptr) {
        lv_label_set_text(m_date_label, date_str);
    }
}

void OrbWidget::update_wifi_status(bool connected)
{
    m_wifi_connected = connected;
    uint32_t color = connected ? COLOR_OK : COLOR_DANGER;
    lv_obj_set_style_bg_color(m_wifi_dot, lv_color_hex(color), 0);
}

void OrbWidget::update_recording_metrics(uint32_t duration_ms, bool speech_detected, uint32_t silence_ms, uint32_t silence_limit_ms)
{
    const uint32_t total_sec = duration_ms / 1000;
    if (total_sec != m_last_total_sec) {
        m_last_total_sec = total_sec;
        char buf[16];
        const uint32_t min = total_sec / 60;
        const uint32_t sec = total_sec % 60;
        snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)min, (unsigned long)sec);
        lv_label_set_text(m_substatus_label, buf);
    }

    // Плавная интерполяция тишины между 64-мс шагами record_task + геп перед появлением дуги
    (void)advance_vad_silence_display(speech_detected,
                                      silence_ms,
                                      silence_limit_ms,
                                      m_last_dt_ms,
                                      &m_last_metric_silence_ms,
                                      &m_display_silence_ms);
    m_last_dt_ms = 0;

    bool visible = false;
    const uint16_t angle = compute_vad_arc_angle(speech_detected, m_display_silence_ms, silence_limit_ms, &visible);
    if (!visible) {
        if (m_arc_visible) {
            m_arc_visible = false;
            m_last_arc_angle = 0;
            lv_obj_add_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (!m_arc_visible) {
            m_arc_visible = true;
            m_last_arc_angle = angle;
            lv_arc_set_angles(m_silence_arc, 0, angle);
            lv_obj_clear_flag(m_silence_arc, LV_OBJ_FLAG_HIDDEN);
        } else if (angle != m_last_arc_angle) {
            m_last_arc_angle = angle;
            lv_arc_set_angles(m_silence_arc, 0, angle);
        }
    }
}

void OrbWidget::update_error_text(const char* text)
{
    if (text != nullptr && text[0] != '\0') {
        lv_label_set_text(m_status_label, text);
    } else {
        lv_label_set_text(m_status_label, "Не вышло");
    }
}

void OrbWidget::update_speech_pulse(bool is_speaking)
{
    if (m_mode != UiVoiceSubStateId::SUB_DONE) {
        return;
    }
    if (is_speaking) {
        // Мягкая пульсация мини-сферы при воспроизведении ответа
        int sz = 32 + (int)(sin((double)m_anim_time_ms * 0.008) * 4.0);
        lv_obj_set_size(m_ring, sz, sz);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, MINI_ORB_TAP_CENTER_Y_OFFSET);
    } else {
        lv_obj_set_size(m_ring, 32, 32);
        lv_obj_align(m_ring, LV_ALIGN_CENTER, 0, MINI_ORB_TAP_CENTER_Y_OFFSET);
    }
}

void OrbWidget::step(uint32_t dt_ms)
{
    m_anim_time_ms += dt_ms;
    m_last_dt_ms = dt_ms;

    if (m_mode == UiVoiceSubStateId::SUB_IDLE) {
        // Дыхание кольца: легкое покачивание толщины границы (2..4 px)
        int width = 3 + (int)(sin((double)m_anim_time_ms * 0.003) * 1.2);
        if (width < 2) width = 2;
        if (width > 4) width = 4;
        lv_obj_set_style_border_width(m_ring, width, 0);
    } else if (m_mode == UiVoiceSubStateId::SUB_SENDING) {
        // Вращение спиннера
        m_spinner_rot = (m_spinner_rot + (dt_ms * 360 / 1000)) % 360;
        lv_arc_set_rotation(m_spinner_arc, m_spinner_rot);
    }
}

} // namespace kouki::ui
