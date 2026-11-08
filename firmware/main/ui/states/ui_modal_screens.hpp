#pragma once

#include "ui_common.hpp"

namespace kouki::ui {

class UiModalScreens {
public:
    explicit UiModalScreens(lv_obj_t* parent);
    ~UiModalScreens();

    void show_setup(const char* ssid, const char* url);
    void show_ota(const char* text);
    void clear_ota();
    bool is_active() const { return m_setup_active || m_ota_active; }

private:
    lv_obj_t* m_parent{nullptr};
    lv_obj_t* m_setup_overlay{nullptr};
    lv_obj_t* m_setup_text{nullptr};
    lv_obj_t* m_ota_overlay{nullptr};
    lv_obj_t* m_ota_text{nullptr};
    lv_obj_t* m_ota_spinner{nullptr};

    bool m_setup_active{false};
    bool m_ota_active{false};
};

} // namespace kouki::ui
