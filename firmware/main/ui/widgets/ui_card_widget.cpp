#include "ui_card_widget.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace kouki::ui {

static void on_card_pressed(lv_event_t* e)
{
    auto* card = static_cast<CardWidget*>(lv_event_get_user_data(e));
    if (card != nullptr) {
        card->stop_auto_scroll();
    }
}

CardWidget::CardWidget(lv_obj_t* parent)
{
    // Контейнер карточки шириной 280px, высотой 230px, по центру по горизонтали, y=65
    m_card = lv_obj_create(parent);
    lv_obj_set_size(m_card, 280, 230);
    lv_obj_align(m_card, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_set_style_bg_color(m_card, lv_color_hex(COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(m_card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(m_card, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_border_width(m_card, 1, 0);
    lv_obj_set_style_radius(m_card, 12, 0);
    lv_obj_set_style_pad_all(m_card, 12, 0);

    // Вертикальная колонка внутри прокручиваемой карточки: исключает наложение при переносе строк
    lv_obj_set_flex_flow(m_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m_card, 6, 0);
    lv_obj_set_scroll_dir(m_card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(m_card, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(m_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m_card, on_card_pressed, LV_EVENT_PRESSED, this);

    // Лейбл бейджа статуса
    m_badge_label = lv_label_create(m_card);
    lv_label_set_text(m_badge_label, "");
    lv_obj_set_style_text_font(m_badge_label, &font_ru_14, 0);
    lv_obj_clear_flag(m_badge_label, LV_OBJ_FLAG_CLICKABLE);

    // Лейбл цитаты распознавания («...»)
    m_transcript_label = lv_label_create(m_card);
    lv_label_set_long_mode(m_transcript_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m_transcript_label, 248);
    lv_obj_set_height(m_transcript_label, LV_SIZE_CONTENT);
    lv_label_set_text(m_transcript_label, "");
    lv_obj_set_style_text_font(m_transcript_label, &font_ru_14, 0);
    lv_obj_set_style_text_color(m_transcript_label, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_text_align(m_transcript_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(m_transcript_label, LV_OBJ_FLAG_CLICKABLE);

    // Лейбл текста ответа
    m_answer_label = lv_label_create(m_card);
    lv_label_set_long_mode(m_answer_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m_answer_label, 248);
    lv_obj_set_height(m_answer_label, LV_SIZE_CONTENT);
    lv_label_set_text(m_answer_label, "");
    lv_obj_set_style_text_font(m_answer_label, &font_ru_14, 0);
    lv_obj_set_style_text_color(m_answer_label, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_align(m_answer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(m_answer_label, LV_OBJ_FLAG_CLICKABLE);

    hide();
}

CardWidget::~CardWidget()
{
    if (m_card != nullptr) {
        lv_obj_del(m_card);
        m_card = nullptr;
    }
}

void CardWidget::set_result(const kouki_voice_result_t& result)
{
    reset_scroll();

    // 1. Бейдж статуса
    if (result.error[0] != '\0') {
        lv_label_set_text(m_badge_label, "[Не вышло]");
        lv_obj_set_style_text_color(m_badge_label, lv_color_hex(COLOR_DANGER), 0);
    } else if (result.answer[0] != '\0') {
        lv_label_set_text(m_badge_label, "[Ответ]");
        lv_obj_set_style_text_color(m_badge_label, lv_color_hex(COLOR_ACCENT), 0);
    } else if (strcmp(result.outcome, "created") == 0) {
        lv_label_set_text(m_badge_label, "[Задача записана]");
        lv_obj_set_style_text_color(m_badge_label, lv_color_hex(COLOR_OK), 0);
    } else if (strcmp(result.outcome, "executed") == 0) {
        lv_label_set_text(m_badge_label, "[Готово]");
        lv_obj_set_style_text_color(m_badge_label, lv_color_hex(COLOR_OK), 0);
    } else {
        lv_label_set_text(m_badge_label, "[На разбор]");
        lv_obj_set_style_text_color(m_badge_label, lv_color_hex(COLOR_WARN), 0);
    }

    constexpr size_t SCRATCH_CAP = 1024;
    std::unique_ptr<char[]> scratch(new (std::nothrow) char[SCRATCH_CAP]);

    // 2. Цитата запроса пользователя
    if (result.has_transcript && result.transcript[0] != '\0') {
        if (scratch) {
            snprintf(scratch.get(), 220, "«%s»", result.transcript);
            utf8_trim_tail(scratch.get());
            lv_label_set_text(m_transcript_label, scratch.get());
        } else {
            lv_label_set_text(m_transcript_label, result.transcript);
        }
        lv_obj_clear_flag(m_transcript_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(m_transcript_label, "");
        lv_obj_add_flag(m_transcript_label, LV_OBJ_FLAG_HIDDEN);
    }

    // 3. Текст результата (один heap-буфер вместо больших стековых массивов в main_task)
    if (scratch) {
        char* text_buf = scratch.get();
        text_buf[0] = '\0';
        if (result.error[0] != '\0') {
            snprintf(text_buf, SCRATCH_CAP, "%s", result.error);
        } else if (result.answer[0] != '\0') {
            snprintf(text_buf, SCRATCH_CAP, "%s", result.answer);
        } else if (result.task_title[0] != '\0') {
            snprintf(text_buf, SCRATCH_CAP, "Задача: %s", result.task_title);
        } else if (result.command[0] != '\0') {
            snprintf(text_buf, SCRATCH_CAP, "Команда: %s", result.command);
        } else if (result.intent[0] != '\0') {
            snprintf(text_buf, SCRATCH_CAP, "Интент: %s", result.intent);
        }
        utf8_trim_tail(text_buf);
        lv_label_set_text(m_answer_label, text_buf);
        m_text_len = static_cast<uint32_t>(strlen(text_buf));
    } else {
        const char* fallback = (result.error[0] != '\0') ? result.error :
                               (result.answer[0] != '\0') ? result.answer :
                               (result.task_title[0] != '\0') ? result.task_title :
                               (result.command[0] != '\0') ? result.command :
                               (result.intent[0] != '\0') ? result.intent : "";
        lv_label_set_text(m_answer_label, fallback);
        m_text_len = static_cast<uint32_t>(strlen(fallback));
    }

    lv_obj_update_layout(m_card);
    m_auto_scroll = (m_text_len > 120);
}

void CardWidget::show()
{
    lv_obj_clear_flag(m_card, LV_OBJ_FLAG_HIDDEN);
}

void CardWidget::hide()
{
    lv_obj_add_flag(m_card, LV_OBJ_FLAG_HIDDEN);
    m_auto_scroll = false;
}

void CardWidget::reset_scroll()
{
    m_scroll_y = 0.0f;
    lv_obj_scroll_to_y(m_card, 0, LV_ANIM_OFF);
}

void CardWidget::step(uint32_t dt_ms)
{
    if (m_auto_scroll) {
        lv_coord_t bottom = lv_obj_get_scroll_bottom(m_card);
        if (bottom <= 0 && m_scroll_y > 10.0f) {
            m_auto_scroll = false;
        } else {
            // Мягкий автоскролл со скоростью 20 px/сек
            m_scroll_y += (20.0f * static_cast<float>(dt_ms)) / 1000.0f;
            lv_obj_scroll_to_y(m_card, static_cast<lv_coord_t>(m_scroll_y), LV_ANIM_OFF);
        }
    }
}

} // namespace kouki::ui
