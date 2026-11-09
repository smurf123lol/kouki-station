#include "ui_history_tile.hpp"
#include "ui_state_manager.hpp"
#include "app_history.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace kouki::ui {

static void on_history_interact(lv_event_t* e)
{
    auto* tile = static_cast<HistoryTile*>(lv_event_get_user_data(e));
    if (tile != nullptr) {
        tile->reset_inactivity();
    }
}

HistoryTile::HistoryTile(lv_obj_t* parent, UiStateManager* manager)
    : m_manager(manager)
{
    m_container = lv_obj_create(parent);
    lv_obj_set_size(m_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(m_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(m_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_container, 0, 0);
    lv_obj_set_style_pad_all(m_container, 0, 0);
    lv_obj_clear_flag(m_container, LV_OBJ_FLAG_SCROLLABLE);

    // Заголовок "История"
    lv_obj_t* title = lv_label_create(m_container);
    lv_label_set_text(title, "История");
    lv_obj_set_style_text_font(title, &font_ru_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // Список карточек
    m_list_container = lv_obj_create(m_container);
    lv_obj_set_size(m_list_container, 310, 290);
    lv_obj_align(m_list_container, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_opa(m_list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_list_container, 0, 0);
    lv_obj_set_style_pad_hor(m_list_container, 15, 0);
    lv_obj_set_style_pad_ver(m_list_container, 5, 0);
    lv_obj_set_flex_flow(m_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(m_list_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(m_list_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(m_list_container, on_history_interact, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(m_list_container, on_history_interact, LV_EVENT_SCROLL, this);

    // Заглушка, если история пуста
    m_empty_label = lv_label_create(m_container);
    lv_label_set_text(m_empty_label, "История пуста");
    lv_obj_set_style_text_font(m_empty_label, &font_ru_16, 0);
    lv_obj_set_style_text_color(m_empty_label, lv_color_hex(COLOR_DIM), 0);
    lv_obj_align(m_empty_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(m_empty_label, LV_OBJ_FLAG_CLICKABLE);

    refresh();
}

HistoryTile::~HistoryTile()
{
    if (m_container != nullptr) {
        lv_obj_del(m_container);
        m_container = nullptr;
    }
}

void HistoryTile::reset_inactivity()
{
    m_inactivity_ms = 0;
}

void HistoryTile::refresh()
{
    // Читаем версию до копирования, но фиксируем m_last_history_version только после успешного выделения буфера
    const uint32_t ver = kouki_history_version();
    const size_t count = kouki_history_count();

    if (count == 0) {
        m_last_history_version = ver;
        lv_obj_clear_flag(m_empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(m_list_container);
        return;
    }

    struct RefreshScratch {
        kouki_history_item_t item;
        char hdr[64];
        char text[520];
    };
    std::unique_ptr<RefreshScratch> scratch(new (std::nothrow) RefreshScratch{});
    if (!scratch) {
        return;
    }
    m_last_history_version = ver;

    lv_obj_add_flag(m_empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(m_list_container);

    for (size_t i = 0; i < count; i++) {
        if (!kouki_history_get(i, &scratch->item)) {
            continue;
        }
        const kouki_history_item_t& item = scratch->item;

        // Карточка записи
        lv_obj_t* card = lv_obj_create(m_list_container);
        lv_obj_set_width(card, 276);
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_PAPER), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Заголовок карточки: "18:05 • Вопрос"
        snprintf(scratch->hdr, sizeof(scratch->hdr), "%s • %s",
                 item.timestamp_hm[0] != '\0' ? item.timestamp_hm : "--:--",
                 item.is_task ? "Задача" : "Вопрос");
        lv_obj_t* lbl_hdr = lv_label_create(card);
        lv_label_set_text(lbl_hdr, scratch->hdr);
        lv_obj_set_style_text_font(lbl_hdr, &font_ru_14, 0);
        lv_obj_set_style_text_color(lbl_hdr, lv_color_hex(item.is_task ? COLOR_OK : COLOR_DIM), 0);

        // Цитата запроса
        if (item.transcript[0] != '\0') {
            snprintf(scratch->text, sizeof(scratch->text), "«%s»", item.transcript);
            utf8_trim_tail(scratch->text);
            lv_obj_t* lbl_q = lv_label_create(card);
            lv_label_set_long_mode(lbl_q, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl_q, 256);
            lv_label_set_text(lbl_q, scratch->text);
            lv_obj_set_style_text_font(lbl_q, &font_ru_14, 0);
            lv_obj_set_style_text_color(lbl_q, lv_color_hex(COLOR_SAND), 0);
        }

        // Ответ или статус
        if (item.is_answer && item.answer[0] != '\0') {
            snprintf(scratch->text, sizeof(scratch->text), "%s", item.answer);
            utf8_trim_tail(scratch->text);
            lv_obj_t* lbl_ans = lv_label_create(card);
            lv_label_set_long_mode(lbl_ans, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl_ans, 256);
            lv_label_set_text(lbl_ans, scratch->text);
            lv_obj_set_style_text_font(lbl_ans, &font_ru_14, 0);
            lv_obj_set_style_text_color(lbl_ans, lv_color_hex(COLOR_DIM), 0);
        } else if (item.is_task) {
            lv_obj_t* lbl_status = lv_label_create(card);
            lv_label_set_text(lbl_status, "Задача записана");
            lv_obj_set_style_text_font(lbl_status, &font_ru_14, 0);
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_OK), 0);
        }
    }
}

void HistoryTile::step(uint32_t dt_ms)
{
    const bool is_active = (m_manager != nullptr && m_manager->get_active_tile() == TileId::TILE_HISTORY);
    if (is_active && !m_was_active && m_list_container != nullptr) {
        lv_obj_scroll_to_y(m_list_container, 0, LV_ANIM_OFF);
    }
    m_was_active = is_active;

    // O(1) проверка изменений без копирования kouki_history_item_t на стек UI-потока
    if (kouki_history_version() != m_last_history_version) {
        refresh();
    }

    // Автовозврат на главный экран через 20 секунд неактивности
    if (is_active) {
        m_inactivity_ms += dt_ms;
        if (m_inactivity_ms >= TILE_INACTIVITY_MS) {
            m_inactivity_ms = 0;
            m_manager->focus_voice_tile(true);
        }
    } else {
        m_inactivity_ms = 0;
    }
}

} // namespace kouki::ui
