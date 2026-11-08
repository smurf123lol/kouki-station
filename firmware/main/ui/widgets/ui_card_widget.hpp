#pragma once

#include "ui_common.hpp"
#include "app_voice.h"

namespace kouki::ui {

class CardWidget {
public:
    explicit CardWidget(lv_obj_t* parent);
    ~CardWidget();

    void set_result(const kouki_voice_result_t& result);
    void show();
    void hide();
    void reset_scroll();
    void step(uint32_t dt_ms);

    lv_obj_t* get_root() const { return m_card; }

private:
    lv_obj_t* m_card{nullptr};
    lv_obj_t* m_badge_label{nullptr};
    lv_obj_t* m_transcript_label{nullptr};
    lv_obj_t* m_answer_label{nullptr};

    bool m_auto_scroll{false};
    float m_scroll_y{0.0f};
    uint32_t m_text_len{0};
};

} // namespace kouki::ui
