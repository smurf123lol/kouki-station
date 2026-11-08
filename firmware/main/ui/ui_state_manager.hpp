#pragma once

#include "ui_common.hpp"
#include "ui_types.hpp"
#include "ui_state_base.hpp"
#include "states/ui_voice_substates.hpp"

namespace kouki::ui {

class OrbWidget;
class CardWidget;
class HistoryTile;
class SettingsTile;
class UiModalScreens;

class UiStateManager {
public:
    static UiStateManager& instance();

    void init();
    void step(uint32_t dt_ms);

    void transition_to(UiVoiceSubStateId next_state);
    void lock_scroll();
    void unlock_scroll();
    void focus_voice_tile(bool anim);

    TileId get_active_tile() const;
    void toggle_recording();

    void show_setup(const char* ssid, const char* url);
    void show_ota_progress(const char* text);
    void clear_ota();

    UiContext& get_context() { return m_ctx; }

private:
    UiStateManager();
    ~UiStateManager();

    UiStateManager(const UiStateManager&) = delete;
    UiStateManager& operator=(const UiStateManager&) = delete;

    UiVoiceSubStateBase* get_substate(UiVoiceSubStateId id);

    lv_obj_t* m_screen{nullptr};
    lv_obj_t* m_tileview{nullptr};
    lv_obj_t* m_tile_history{nullptr};
    lv_obj_t* m_tile_voice{nullptr};
    lv_obj_t* m_tile_settings{nullptr};

    OrbWidget* m_orb{nullptr};
    CardWidget* m_response_card{nullptr};
    HistoryTile* m_history_tile{nullptr};
    SettingsTile* m_settings_tile{nullptr};
    UiModalScreens* m_modal_screens{nullptr};

    SubIdleState m_sub_idle;
    SubRecordingState m_sub_recording;
    SubSendingState m_sub_sending;
    SubDoneState m_sub_done;
    SubFollowupState m_sub_followup;
    SubErrorState m_sub_error;

    UiVoiceSubStateBase* m_current_substate{nullptr};
    kouki_voice_state_t m_last_voice_state{KOUKI_VOICE_IDLE};

    UiContext m_ctx{};
    bool m_initialized{false};
};

} // namespace kouki::ui
