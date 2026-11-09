#pragma once

#include "ui_common.hpp"
#include <atomic>

namespace kouki::ui {

class UiModalScreens {
public:
    explicit UiModalScreens(lv_obj_t* parent);
    ~UiModalScreens();

    void show_setup(const char* ssid, const char* url);
    void clear_setup();
    void show_ota(const char* text);
    void clear_ota();
    bool is_active() const { return m_setup_active.load(std::memory_order_acquire) || m_ota_active.load(std::memory_order_acquire); }
    bool is_setup_active() const { return m_setup_active.load(std::memory_order_acquire); }
    bool is_ota_active() const { return m_ota_active.load(std::memory_order_acquire); }

private:
    lv_obj_t* m_parent{nullptr};
    lv_obj_t* m_setup_overlay{nullptr};
    lv_obj_t* m_setup_text{nullptr};
    lv_obj_t* m_ota_overlay{nullptr};
    lv_obj_t* m_ota_text{nullptr};
    lv_obj_t* m_ota_spinner{nullptr};

    std::atomic<bool> m_setup_active{false};
    std::atomic<bool> m_ota_active{false};
};

} // namespace kouki::ui
