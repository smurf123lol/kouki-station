#pragma once

#include "ui_common.hpp"

namespace kouki::ui {

class UiStateManager;

class HistoryTile {
public:
    HistoryTile(lv_obj_t* parent, UiStateManager* manager);
    ~HistoryTile();

    void refresh();
    void reset_inactivity();
    void step(uint32_t dt_ms);

    lv_obj_t* get_root() const { return m_container; }

private:
    lv_obj_t* m_container{nullptr};
    lv_obj_t* m_list_container{nullptr};
    lv_obj_t* m_empty_label{nullptr};
    UiStateManager* m_manager{nullptr};

    uint32_t m_inactivity_ms{0};
    size_t m_last_history_count{0};
};

} // namespace kouki::ui
