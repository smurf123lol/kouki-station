#include "ui_state_manager.hpp"
#include "widgets/ui_orb_widget.hpp"
#include "widgets/ui_card_widget.hpp"
#include "states/ui_history_tile.hpp"
#include "states/ui_settings_tile.hpp"
#include "states/ui_modal_screens.hpp"
#include "app_voice.h"
#include "app_speech.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kouki.ui.mgr";

namespace kouki::ui {

static void on_tile_voice_event_cb(lv_event_t* e)
{
    auto* mgr = static_cast<UiStateManager*>(lv_event_get_user_data(e));
    if (mgr != nullptr) {
        mgr->on_tile_voice_event(e);
    }
}

UiStateManager::UiStateManager() = default;
UiStateManager::~UiStateManager()
{
    delete m_orb;
    delete m_response_card;
    delete m_history_tile;
    delete m_settings_tile;
    delete m_modal_screens;
}

UiStateManager& UiStateManager::instance()
{
    static UiStateManager s_instance;
    return s_instance;
}

void UiStateManager::on_voice_tile_clicked()
{
    if (m_current_substate != nullptr) {
        m_current_substate->on_tap(m_ctx);
    }
}

void UiStateManager::on_tile_voice_event(lv_event_t* e)
{
    if (e == nullptr || lv_event_get_target(e) != m_tile_voice) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED &&
        code != LV_EVENT_PRESSING &&
        code != LV_EVENT_PRESS_LOST &&
        code != LV_EVENT_CLICKED) {
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        m_press_active = false;
        m_press_in_zone = false;
        m_press_dragged = false;
        return;
    }

    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        indev = lv_indev_get_act();
    }
    if (indev == nullptr) {
        if (code == LV_EVENT_PRESSED) {
            m_press_active = false;
            m_press_in_zone = false;
            m_press_dragged = false;
        }
        return;
    }

    lv_point_t pt{};
    lv_indev_get_point(indev, &pt);

    lv_area_t coords{};
    lv_obj_get_coords(m_tile_voice, &coords);
    const int cx = (static_cast<int>(coords.x1) + static_cast<int>(coords.x2)) / 2;
    const int cy = (static_cast<int>(coords.y1) + static_cast<int>(coords.y2)) / 2;
    const bool done_mode = (m_current_substate != nullptr && m_current_substate->id() == UiVoiceSubStateId::SUB_DONE);

    if (code == LV_EVENT_PRESSED) {
        m_press_active = true;
        m_press_dragged = false;
        m_press_pt_x = pt.x;
        m_press_pt_y = pt.y;
        m_press_in_zone = (get_active_tile() == TileId::TILE_VOICE) &&
                          is_in_voice_tap_zone(static_cast<int>(pt.x) - cx, static_cast<int>(pt.y) - cy, done_mode);
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (m_press_active && !m_press_dragged) {
            if (is_drag_beyond_threshold(static_cast<int>(pt.x) - m_press_pt_x, static_cast<int>(pt.y) - m_press_pt_y) ||
                lv_indev_get_scroll_obj(indev) != nullptr) {
                m_press_dragged = true;
            }
        }
        return;
    }

    // LV_EVENT_CLICKED: пропускаем только чистый тап в зоне без свайпа/скролла
    const bool should_tap = m_press_active &&
                            m_press_in_zone &&
                            !m_press_dragged &&
                            (get_active_tile() == TileId::TILE_VOICE) &&
                            (lv_indev_get_scroll_obj(indev) == nullptr) &&
                            !is_drag_beyond_threshold(static_cast<int>(pt.x) - m_press_pt_x, static_cast<int>(pt.y) - m_press_pt_y) &&
                            is_in_voice_tap_zone(static_cast<int>(pt.x) - cx, static_cast<int>(pt.y) - cy, done_mode);

    m_press_active = false;
    m_press_in_zone = false;
    m_press_dragged = false;

    if (should_tap) {
        on_voice_tile_clicked();
    }
}

void UiStateManager::init()
{
    if (m_initialized) {
        return;
    }

    m_screen = lv_scr_act();
    lv_obj_set_style_bg_color(m_screen, lv_color_hex(COLOR_INK), 0);

    // Главный навигатор по плиткам (lv_tileview)
    m_tileview = lv_tileview_create(m_screen);
    lv_obj_set_size(m_tileview, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(m_tileview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(m_tileview, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_border_width(m_tileview, 0, 0);

    // Добавляем 3 горизонтальные плитки:
    // Tile 0: История (col=0, row=0, свайп только вправо на Home)
    m_tile_history = lv_tileview_add_tile(m_tileview, 0, 0, LV_DIR_RIGHT);
    // Tile 1: Главная Сфера (col=1, row=0, свайп влево и вправо)
    m_tile_voice = lv_tileview_add_tile(m_tileview, 1, 0, LV_DIR_HOR);
    // Tile 2: Настройки (col=2, row=0, свайп только влево на Home)
    m_tile_settings = lv_tileview_add_tile(m_tileview, 2, 0, LV_DIR_LEFT);

    // По умолчанию фокус на центральном экране (Home / Voice Orb)
    lv_obj_set_tile_id(m_tileview, 1, 0, LV_ANIM_OFF);

    // Создаем виджеты Сферы и Карточки на центральной плитке
    m_orb = new OrbWidget(m_tile_voice);
    m_response_card = new CardWidget(m_tile_voice);

    // Создаем экраны Истории и Настроек на соответствующих плитках
    m_history_tile = new HistoryTile(m_tile_history, this);
    m_settings_tile = new SettingsTile(m_tile_settings, this);

    // Модальные экраны первичной настройки и OTA поверх экрана
    m_modal_screens = new UiModalScreens(m_screen);

    // Инициализация контекста UI
    m_ctx.manager = this;
    m_ctx.tileview = m_tileview;
    m_ctx.tile_history = m_tile_history;
    m_ctx.tile_voice = m_tile_voice;
    m_ctx.tile_settings = m_tile_settings;
    m_ctx.orb = m_orb;
    m_ctx.response_card = m_response_card;

    // Разделение тапа и свайпа по центральному экрану
    lv_obj_add_event_cb(m_tile_voice, on_tile_voice_event_cb, LV_EVENT_ALL, this);

    // Стартуем в подсостоянии SubIdle
    m_current_substate = &m_sub_idle;
    m_current_substate->on_enter(m_ctx);

    m_initialized = true;
    ESP_LOGI(TAG, "UI State Machine инициализирована успешно (3 плитки)");
}

UiVoiceSubStateBase* UiStateManager::get_substate(UiVoiceSubStateId id)
{
    switch (id) {
    case UiVoiceSubStateId::SUB_IDLE:      return &m_sub_idle;
    case UiVoiceSubStateId::SUB_RECORDING: return &m_sub_recording;
    case UiVoiceSubStateId::SUB_SENDING:   return &m_sub_sending;
    case UiVoiceSubStateId::SUB_DONE:      return &m_sub_done;
    case UiVoiceSubStateId::SUB_FOLLOWUP:  return &m_sub_followup;
    case UiVoiceSubStateId::SUB_ERROR:     return &m_sub_error;
    default:                               return &m_sub_idle;
    }
}

void UiStateManager::transition_to(UiVoiceSubStateId next_state)
{
    if (m_current_substate != nullptr && m_current_substate->id() == next_state) {
        return;
    }

    m_press_active = false;
    m_press_in_zone = false;
    m_press_dragged = false;

    if (m_current_substate != nullptr) {
        m_current_substate->on_exit(m_ctx);
    }

    m_current_substate = get_substate(next_state);
    if (m_current_substate != nullptr) {
        m_current_substate->on_enter(m_ctx);
    }
}

void UiStateManager::lock_scroll()
{
    if (m_tileview != nullptr) {
        lv_obj_clear_flag(m_tileview, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void UiStateManager::unlock_scroll()
{
    if (m_tileview != nullptr) {
        lv_obj_add_flag(m_tileview, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void UiStateManager::focus_voice_tile(bool anim)
{
    if (m_tileview != nullptr) {
        lv_obj_set_tile_id(m_tileview, 1, 0, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

TileId UiStateManager::get_active_tile() const
{
    if (m_tileview == nullptr) {
        return TileId::TILE_VOICE;
    }
    lv_obj_t* act = lv_tileview_get_tile_act(m_tileview);
    if (act == m_tile_history) return TileId::TILE_HISTORY;
    if (act == m_tile_settings) return TileId::TILE_SETTINGS;
    return TileId::TILE_VOICE;
}

void UiStateManager::toggle_recording()
{
    if (m_modal_screens != nullptr && m_modal_screens->is_active()) {
        return;
    }

    if (kouki_voice_is_recording()) {
        ESP_LOGI(TAG, "останавливаю запись");
        kouki_voice_stop_and_send();
        return;
    }

    if (kouki_speech_is_playing()) {
        ESP_LOGI(TAG, "прерываю озвучивание перед новой записью");
        kouki_speech_abort();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    const esp_err_t start_err = kouki_voice_start_recording();
    if (start_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "запись не началась: станция занята");
    } else if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "запись не началась: %s", esp_err_to_name(start_err));
    } else {
        ESP_LOGI(TAG, "начинаю запись");
    }
}

void UiStateManager::show_setup(const char* ssid, const char* url)
{
    if (m_modal_screens != nullptr) {
        m_modal_screens->show_setup(ssid, url);
    }
}

void UiStateManager::show_ota_progress(const char* text)
{
    if (m_modal_screens != nullptr) {
        m_modal_screens->show_ota(text);
    }
}

void UiStateManager::clear_ota()
{
    if (m_modal_screens != nullptr) {
        m_modal_screens->clear_ota();
    }
}

void UiStateManager::step(uint32_t dt_ms)
{
    if (!m_initialized) {
        return;
    }

    // Получаем текущий снимок состояния голоса и метрик VAD; если m_lock занят, сохраняем предыдущие значения
    kouki_voice_state_t v_state = m_last_voice_state;
    kouki_voice_get_snapshot(&v_state, &m_ctx.voice_result);
    kouki_voice_get_metrics(&m_ctx.voice_metrics);
    m_ctx.voice_state = v_state;

    // Реакция на изменение голосового состояния
    if (v_state != m_last_voice_state) {
        m_last_voice_state = v_state;

        switch (v_state) {
        case KOUKI_VOICE_IDLE:
            transition_to(UiVoiceSubStateId::SUB_IDLE);
            break;
        case KOUKI_VOICE_RECORDING:
            focus_voice_tile(false); // Приоритет «эй гин» / мгновенный возврат на Tile 1
            transition_to(UiVoiceSubStateId::SUB_RECORDING);
            break;
        case KOUKI_VOICE_SENDING:
            transition_to(UiVoiceSubStateId::SUB_SENDING);
            break;
        case KOUKI_VOICE_DONE:
            transition_to(UiVoiceSubStateId::SUB_DONE);
            break;
        case KOUKI_VOICE_FOLLOWUP:
            transition_to(UiVoiceSubStateId::SUB_FOLLOWUP);
            break;
        case KOUKI_VOICE_ERROR:
            transition_to(UiVoiceSubStateId::SUB_ERROR);
            break;
        }
    }

    if (m_current_substate != nullptr) {
        m_current_substate->step(m_ctx, dt_ms);
    }
    if (m_history_tile != nullptr) {
        m_history_tile->step(dt_ms);
    }
    if (m_settings_tile != nullptr) {
        m_settings_tile->step(dt_ms);
    }
}

} // namespace kouki::ui
