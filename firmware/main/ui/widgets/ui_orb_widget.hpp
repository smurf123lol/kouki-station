#pragma once

#include "ui_common.hpp"
#include "ui_types.hpp"

namespace kouki::ui {

class OrbWidget {
public:
    explicit OrbWidget(lv_obj_t* parent);
    ~OrbWidget();

    void set_mode(UiVoiceSubStateId mode);
    void update_time(const char* hm_str, const char* date_str);
    void update_wifi_status(bool connected);
    void update_recording_metrics(uint32_t duration_ms, bool speech_detected, uint32_t silence_ms, uint32_t silence_limit_ms);
    void update_error_text(const char* text);
    void update_speech_pulse(bool is_speaking);
    void step(uint32_t dt_ms);

    lv_obj_t* get_root() const { return m_container; }

private:
    lv_obj_t* m_container{nullptr};
    lv_obj_t* m_ring{nullptr};
    lv_obj_t* m_silence_arc{nullptr};
    lv_obj_t* m_spinner_arc{nullptr};
    lv_obj_t* m_time_label{nullptr};
    lv_obj_t* m_date_label{nullptr};
    lv_obj_t* m_status_label{nullptr};
    lv_obj_t* m_substatus_label{nullptr};
    lv_obj_t* m_wifi_dot{nullptr};

    UiVoiceSubStateId m_mode{UiVoiceSubStateId::SUB_IDLE};
    uint32_t m_anim_time_ms{0};
    uint16_t m_spinner_rot{0};
    bool m_wifi_connected{false};
};

} // namespace kouki::ui
