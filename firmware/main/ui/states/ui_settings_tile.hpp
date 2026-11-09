#pragma once

#include "ui_common.hpp"

namespace kouki::ui {

class UiStateManager;

class SettingsTile {
public:
    SettingsTile(lv_obj_t* parent, UiStateManager* manager);
    ~SettingsTile();

    void refresh_wifi();
    void reset_inactivity();
    void update_vol_label(int val);
    void update_wake_sens_label(int val);
    void step(uint32_t dt_ms);

    lv_obj_t* get_root() const { return m_container; }

private:
    lv_obj_t* m_container{nullptr};
    lv_obj_t* m_scroll_container{nullptr};
    lv_obj_t* m_vol_label{nullptr};
    lv_obj_t* m_vol_slider{nullptr};
    lv_obj_t* m_wake_sw{nullptr};
    lv_obj_t* m_wake_sens_label{nullptr};
    lv_obj_t* m_wake_slider{nullptr};
    lv_obj_t* m_silent_sw{nullptr};
    lv_obj_t* m_wifi_label{nullptr};
    lv_obj_t* m_ota_btn_label{nullptr};

    UiStateManager* m_manager{nullptr};
    uint32_t m_inactivity_ms{0};
    uint32_t m_wifi_poll_ms{0};
    uint32_t m_last_wifi_seq{0};
    bool m_was_active{false};
};

} // namespace kouki::ui
