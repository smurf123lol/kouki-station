#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct lv_font_t {
    int size;
};

#define LV_FONT_DECLARE(name) extern const lv_font_t name

struct lv_color_t {
    uint32_t value;
};

inline lv_color_t lv_color_hex(uint32_t c)
{
    return lv_color_t{c};
}

typedef int16_t lv_coord_t;

struct lv_point_t {
    lv_coord_t x;
    lv_coord_t y;
};

struct lv_area_t {
    lv_coord_t x1;
    lv_coord_t y1;
    lv_coord_t x2;
    lv_coord_t y2;
};

typedef uint32_t lv_style_selector_t;

enum lv_anim_enable_t : uint8_t {
    LV_ANIM_OFF = 0,
    LV_ANIM_ON = 1,
};

enum lv_dir_t : uint8_t {
    LV_DIR_NONE = 0x00,
    LV_DIR_LEFT = (1 << 0),
    LV_DIR_RIGHT = (1 << 1),
    LV_DIR_TOP = (1 << 2),
    LV_DIR_BOTTOM = (1 << 3),
    LV_DIR_HOR = LV_DIR_LEFT | LV_DIR_RIGHT,
    LV_DIR_VER = LV_DIR_TOP | LV_DIR_BOTTOM,
};

enum lv_obj_flag_t : uint32_t {
    LV_OBJ_FLAG_HIDDEN = (1u << 0),
    LV_OBJ_FLAG_CLICKABLE = (1u << 1),
    LV_OBJ_FLAG_SCROLLABLE = (1u << 5),
};

enum lv_part_t : uint32_t {
    LV_PART_MAIN = 0x000000u,
    LV_PART_INDICATOR = 0x020000u,
    LV_PART_KNOB = 0x030000u,
};

enum lv_state_t : uint32_t {
    LV_STATE_DEFAULT = 0x0000u,
    LV_STATE_CHECKED = 0x0001u,
};

enum lv_opa_t : uint8_t {
    LV_OPA_TRANSP = 0,
    LV_OPA_90 = 230,
    LV_OPA_100 = 255,
};

enum lv_align_t : uint8_t {
    LV_ALIGN_DEFAULT = 0,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_LEFT_MID,
    LV_ALIGN_RIGHT_MID,
    LV_ALIGN_CENTER,
};

enum lv_text_align_t : uint8_t {
    LV_TEXT_ALIGN_AUTO = 0,
    LV_TEXT_ALIGN_LEFT,
    LV_TEXT_ALIGN_CENTER,
    LV_TEXT_ALIGN_RIGHT,
};

enum lv_label_long_mode_t : uint8_t {
    LV_LABEL_LONG_WRAP = 0,
};

enum lv_flex_flow_t : uint8_t {
    LV_FLEX_FLOW_COLUMN = 1,
};

enum lv_flex_align_t : uint8_t {
    LV_FLEX_ALIGN_START = 0,
    LV_FLEX_ALIGN_END,
    LV_FLEX_ALIGN_CENTER,
};

enum lv_scrollbar_mode_t : uint8_t {
    LV_SCROLLBAR_MODE_AUTO = 1,
};

enum lv_event_code_t : uint32_t {
    LV_EVENT_ALL = 0,
    LV_EVENT_PRESSED,
    LV_EVENT_PRESSING,
    LV_EVENT_PRESS_LOST,
    LV_EVENT_CLICKED,
    LV_EVENT_VALUE_CHANGED,
    LV_EVENT_RELEASED,
    LV_EVENT_SCROLL,
};

constexpr lv_coord_t LV_RADIUS_CIRCLE = 0x7FFF;
constexpr lv_coord_t LV_SIZE_CONTENT = 0x2001;

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct lv_indev_t;
struct lv_event_t;

typedef void (*lv_event_cb_t)(lv_event_t *e);

struct lv_mock_listener_t {
    lv_event_cb_t cb{nullptr};
    lv_event_code_t filter{LV_EVENT_ALL};
    void *user_data{nullptr};
};

typedef struct _lv_obj_t {
    struct _lv_obj_t *parent{nullptr};
    std::vector<struct _lv_obj_t *> children;
    uint32_t flags{LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE};
    uint32_t state{LV_STATE_DEFAULT};
    lv_area_t area{0, 0, 359, 359};
    lv_coord_t w{360};
    lv_coord_t h{360};
    uint32_t bg_color{0};
    uint8_t bg_opa{LV_OPA_100};
    uint32_t border_color{0};
    lv_coord_t border_width{0};
    lv_coord_t radius{0};
    uint16_t arc_rotation{0};
    uint16_t arc_bg_start{0};
    uint16_t arc_bg_end{360};
    uint16_t arc_start{0};
    uint16_t arc_end{0};
    std::string text;
    int32_t slider_min{0};
    int32_t slider_max{100};
    int32_t slider_val{0};
    lv_coord_t scroll_y{0};
    lv_coord_t scroll_bottom{0};
    bool is_tileview{false};
    uint32_t active_tile_col{0};
    std::vector<struct _lv_obj_t *> tile_cols;
    std::vector<lv_mock_listener_t> listeners;
} lv_obj_t;

struct lv_indev_t {
    lv_point_t point{0, 0};
    lv_obj_t *scroll_obj{nullptr};
};

struct lv_event_t {
    lv_event_code_t code{LV_EVENT_ALL};
    lv_obj_t *target{nullptr};
    void *user_data{nullptr};
    lv_indev_t *indev{nullptr};
};

namespace lv_mock_state {
inline lv_obj_t *g_screen = nullptr;
inline lv_indev_t g_default_indev{};
inline lv_indev_t *g_act_indev = &g_default_indev;

inline void delete_children(lv_obj_t *obj)
{
    if (!obj) return;
    auto kids = obj->children;
    obj->children.clear();
    for (lv_obj_t *ch : kids) {
        delete_children(ch);
        ch->parent = nullptr;
        delete ch;
    }
}
} // namespace lv_mock_state

inline lv_obj_t *lv_scr_act()
{
    if (lv_mock_state::g_screen == nullptr) {
        lv_mock_state::g_screen = new lv_obj_t{};
    }
    return lv_mock_state::g_screen;
}

inline lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    auto *obj = new lv_obj_t{};
    obj->parent = parent;
    obj->area = lv_area_t{0, 0, 359, 359};
    if (parent != nullptr) {
        parent->children.push_back(obj);
    }
    return obj;
}

inline lv_obj_t *lv_tileview_create(lv_obj_t *parent)
{
    lv_obj_t *tv = lv_obj_create(parent);
    tv->is_tileview = true;
    return tv;
}

inline lv_obj_t *lv_tileview_add_tile(lv_obj_t *tv, uint8_t col_id, uint8_t /*row_id*/, uint8_t /*dir*/)
{
    lv_obj_t *tile = lv_obj_create(tv);
    tile->area = lv_area_t{0, 0, 359, 359};
    if (tv != nullptr) {
        if (tv->tile_cols.size() <= col_id) {
            tv->tile_cols.resize(col_id + 1, nullptr);
        }
        tv->tile_cols[col_id] = tile;
    }
    return tile;
}

inline void lv_obj_set_tile_id(lv_obj_t *tv, uint32_t col_id, uint32_t /*row_id*/, lv_anim_enable_t /*anim*/)
{
    if (tv != nullptr && col_id < tv->tile_cols.size()) {
        tv->active_tile_col = col_id;
    }
}

inline lv_obj_t *lv_tileview_get_tile_act(lv_obj_t *tv)
{
    if (tv == nullptr || tv->active_tile_col >= tv->tile_cols.size()) {
        return nullptr;
    }
    return tv->tile_cols[tv->active_tile_col];
}

inline lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_arc_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_slider_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_switch_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_btn_create(lv_obj_t *parent) { return lv_obj_create(parent); }

inline void lv_obj_clean(lv_obj_t *obj)
{
    lv_mock_state::delete_children(obj);
}

inline void lv_obj_del(lv_obj_t *obj)
{
    if (!obj) return;
    if (obj->parent != nullptr) {
        auto &sibs = obj->parent->children;
        sibs.erase(std::remove(sibs.begin(), sibs.end(), obj), sibs.end());
    }
    lv_mock_state::delete_children(obj);
    if (obj == lv_mock_state::g_screen) {
        lv_mock_state::g_screen = nullptr;
    }
    delete obj;
}

inline void lv_obj_set_size(lv_obj_t *obj, lv_coord_t w, lv_coord_t h)
{
    if (!obj) return;
    obj->w = w;
    obj->h = h;
    if (w > 0 && h > 0 && w != LV_SIZE_CONTENT && h != LV_SIZE_CONTENT) {
        obj->area.x2 = static_cast<lv_coord_t>(obj->area.x1 + w - 1);
        obj->area.y2 = static_cast<lv_coord_t>(obj->area.y1 + h - 1);
    }
}

inline void lv_obj_set_width(lv_obj_t *obj, lv_coord_t w) { if (obj) obj->w = w; }
inline void lv_obj_set_height(lv_obj_t *obj, lv_coord_t h) { if (obj) obj->h = h; }
inline void lv_obj_align(lv_obj_t * /*obj*/, lv_align_t /*align*/, lv_coord_t /*x_ofs*/, lv_coord_t /*y_ofs*/) {}
inline void lv_obj_center(lv_obj_t * /*obj*/) {}

inline void lv_obj_get_coords(const lv_obj_t *obj, lv_area_t *coords)
{
    if (coords && obj) {
        *coords = obj->area;
    }
}

inline void lv_obj_add_flag(lv_obj_t *obj, uint32_t f) { if (obj) obj->flags |= f; }
inline void lv_obj_clear_flag(lv_obj_t *obj, uint32_t f) { if (obj) obj->flags &= ~f; }
inline bool lv_obj_has_flag(const lv_obj_t *obj, uint32_t f) { return obj && ((obj->flags & f) == f); }

inline void lv_obj_add_state(lv_obj_t *obj, uint32_t state) { if (obj) obj->state |= state; }
inline void lv_obj_clear_state(lv_obj_t *obj, uint32_t state) { if (obj) obj->state &= ~state; }
inline bool lv_obj_has_state(const lv_obj_t *obj, uint32_t state) { return obj && ((obj->state & state) == state); }

inline void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t val, lv_style_selector_t sel)
{
    if (obj && (sel & 0xFFFF0000u) == LV_PART_MAIN) obj->bg_color = val.value;
}
inline void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t val, lv_style_selector_t sel)
{
    if (obj && (sel & 0xFFFF0000u) == LV_PART_MAIN) obj->bg_opa = val;
}
inline void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t val, lv_style_selector_t /*sel*/)
{
    if (obj) obj->border_color = val.value;
}
inline void lv_obj_set_style_border_width(lv_obj_t *obj, lv_coord_t val, lv_style_selector_t /*sel*/)
{
    if (obj) obj->border_width = val;
}
inline void lv_obj_set_style_radius(lv_obj_t *obj, lv_coord_t val, lv_style_selector_t /*sel*/)
{
    if (obj) obj->radius = val;
}
inline void lv_obj_set_style_pad_all(lv_obj_t *, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_pad_hor(lv_obj_t *, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_pad_ver(lv_obj_t *, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_pad_row(lv_obj_t *, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_text_font(lv_obj_t *, const lv_font_t *, lv_style_selector_t) {}
inline void lv_obj_set_style_text_color(lv_obj_t *, lv_color_t, lv_style_selector_t) {}
inline void lv_obj_set_style_text_align(lv_obj_t *, lv_text_align_t, lv_style_selector_t) {}
inline void lv_obj_set_style_arc_opa(lv_obj_t *, lv_opa_t, lv_style_selector_t) {}
inline void lv_obj_set_style_arc_color(lv_obj_t *, lv_color_t, lv_style_selector_t) {}
inline void lv_obj_set_style_arc_width(lv_obj_t *, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_remove_style(lv_obj_t *, const void *, lv_style_selector_t) {}
inline void lv_obj_set_flex_flow(lv_obj_t *, lv_flex_flow_t) {}
inline void lv_obj_set_flex_align(lv_obj_t *, lv_flex_align_t, lv_flex_align_t, lv_flex_align_t) {}
inline void lv_obj_update_layout(const lv_obj_t *) {}
inline void lv_obj_set_scroll_dir(lv_obj_t *, uint8_t) {}
inline void lv_obj_set_scrollbar_mode(lv_obj_t *, lv_scrollbar_mode_t) {}

inline void lv_obj_scroll_to_y(lv_obj_t *obj, lv_coord_t y, lv_anim_enable_t /*anim*/)
{
    if (obj) {
        obj->scroll_y = y;
    }
}

inline lv_coord_t lv_obj_get_scroll_bottom(const lv_obj_t *obj)
{
    if (!obj) return 0;
    return static_cast<lv_coord_t>(obj->scroll_bottom - obj->scroll_y);
}

inline void lv_arc_set_rotation(lv_obj_t *obj, uint16_t rot) { if (obj) obj->arc_rotation = rot; }
inline void lv_arc_set_bg_angles(lv_obj_t *obj, uint16_t s, uint16_t e) { if (obj) { obj->arc_bg_start = s; obj->arc_bg_end = e; } }
inline void lv_arc_set_angles(lv_obj_t *obj, uint16_t s, uint16_t e) { if (obj) { obj->arc_start = s; obj->arc_end = e; } }

inline void lv_label_set_text(lv_obj_t *obj, const char *txt) { if (obj) obj->text = (txt ? txt : ""); }
inline void lv_label_set_long_mode(lv_obj_t *, lv_label_long_mode_t) {}

inline void lv_slider_set_range(lv_obj_t *obj, int32_t min, int32_t max) { if (obj) { obj->slider_min = min; obj->slider_max = max; } }
inline void lv_slider_set_value(lv_obj_t *obj, int32_t val, lv_anim_enable_t /*anim*/) { if (obj) obj->slider_val = val; }
inline int32_t lv_slider_get_value(const lv_obj_t *obj) { return obj ? obj->slider_val : 0; }

inline void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t filter, void *user_data)
{
    if (obj && cb) {
        obj->listeners.push_back(lv_mock_listener_t{cb, filter, user_data});
    }
}

inline lv_event_code_t lv_event_get_code(lv_event_t *e) { return e ? e->code : LV_EVENT_ALL; }
inline lv_obj_t *lv_event_get_target(lv_event_t *e) { return e ? e->target : nullptr; }
inline void *lv_event_get_user_data(lv_event_t *e) { return e ? e->user_data : nullptr; }
inline lv_indev_t *lv_event_get_indev(lv_event_t *e) { return e ? e->indev : nullptr; }

inline lv_indev_t *lv_indev_get_act() { return lv_mock_state::g_act_indev; }
inline void lv_indev_get_point(const lv_indev_t *indev, lv_point_t *pt)
{
    if (pt) {
        *pt = indev ? indev->point : lv_point_t{0, 0};
    }
}
inline lv_obj_t *lv_indev_get_scroll_obj(const lv_indev_t *indev)
{
    return indev ? indev->scroll_obj : nullptr;
}

inline void lv_mock_send_event(lv_obj_t *target, lv_event_code_t code, lv_indev_t *indev = nullptr)
{
    if (!target) return;
    lv_indev_t *eff_indev = indev ? indev : lv_mock_state::g_act_indev;
    const auto listeners = target->listeners;
    for (const auto &l : listeners) {
        if (l.filter == LV_EVENT_ALL || l.filter == code) {
            lv_event_t ev{};
            ev.code = code;
            ev.target = target;
            ev.user_data = l.user_data;
            ev.indev = eff_indev;
            l.cb(&ev);
        }
    }
}
