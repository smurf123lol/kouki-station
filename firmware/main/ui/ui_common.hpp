#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef ESP_PLATFORM
#include "lvgl.h"
LV_FONT_DECLARE(font_ru_14);
LV_FONT_DECLARE(font_ru_16);
LV_FONT_DECLARE(font_ru_20);
#elif __has_include("lvgl_mock.hpp")
#include "lvgl_mock.hpp"
LV_FONT_DECLARE(font_ru_14);
LV_FONT_DECLARE(font_ru_16);
LV_FONT_DECLARE(font_ru_20);
#endif

namespace kouki::ui {

constexpr uint32_t COLOR_INK      = 0x0B0A08;
constexpr uint32_t COLOR_PAPER    = 0x16130F;
constexpr uint32_t COLOR_SAND     = 0xE2D2B2;
constexpr uint32_t COLOR_DIM      = 0x8B7D66;
constexpr uint32_t COLOR_ACCENT   = 0xD8552F;
constexpr uint32_t COLOR_OK       = 0x38D483;
constexpr uint32_t COLOR_WARN     = 0xFFCC33;
constexpr uint32_t COLOR_DANGER   = 0xFF4D6D;

constexpr int SCREEN_WIDTH        = 360;
constexpr int SCREEN_HEIGHT       = 360;
constexpr int MAX_LABEL_WIDTH     = 300;

// Зоны взаимодействия и тайминги UI (единые константы для прошивки и host-тестов)
constexpr int TAP_ZONE_RADIUS              = 120;   // Внутренний круг сферы (диаметр 240 из 360 px)
constexpr int MINI_ORB_TAP_CENTER_Y_OFFSET = -142;  // Центр мини-сферы (y=38) относительно центра 360x360
constexpr int MINI_ORB_TAP_RADIUS          = 40;    // Комфортная зона тапа по мини-сфере в SubDone
constexpr int SWIPE_DRAG_THRESHOLD_PX      = 14;    // Сдвиг >14 px считается свайпом, а не тапом
constexpr uint32_t VAD_ARC_GAP_MS          = 800;   // Пауза тишины до появления дуги отсчета
constexpr uint32_t VAD_INTERP_MAX_AHEAD_MS = 96;    // Ограничение упреждения интерполяции между 64-мс шагами
constexpr uint32_t WIFI_RSSI_POLL_MS       = 10000; // Фоновый опрос RSSI (10 с; смена сети/IP реактивна)
constexpr uint32_t TILE_INACTIVITY_MS      = 20000; // Автовозврат на главный экран

/* Обрезает недописанный UTF-8 символ в конце строки при усечении */
inline void utf8_trim_tail(char *s)
{
    if (s == nullptr) {
        return;
    }
    const size_t len = strlen(s);
    size_t start = len;
    size_t back = 0;
    while (start > 0 && back < 3 && ((unsigned char)s[start - 1] & 0xC0) == 0x80) {
        start--;
        back++;
    }
    if (start == 0) {
        return;
    }
    const unsigned char lead = (unsigned char)s[start - 1];
    size_t need = 0;
    if ((lead & 0x80) == 0) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    if (need > 0 && (len - (start - 1)) < need) {
        s[start - 1] = '\0';
    }
}

/* Проверяет, превысило ли смещение пальца порог свайпа (нельзя трактовать как тап) */
inline bool is_drag_beyond_threshold(int dx, int dy)
{
    const int64_t dist_sq = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
    const int64_t thr_sq = static_cast<int64_t>(SWIPE_DRAG_THRESHOLD_PX) * SWIPE_DRAG_THRESHOLD_PX;
    return dist_sq > thr_sq;
}

/* Проверяет попадание точки относительно центра экрана в активную зону тапа */
inline bool is_in_voice_tap_zone(int dx_center, int dy_center, bool done_mode)
{
    if (done_mode) {
        const int dy_mini = dy_center - MINI_ORB_TAP_CENTER_Y_OFFSET;
        const int64_t dist_sq = static_cast<int64_t>(dx_center) * dx_center + static_cast<int64_t>(dy_mini) * dy_mini;
        return dist_sq <= static_cast<int64_t>(MINI_ORB_TAP_RADIUS) * MINI_ORB_TAP_RADIUS;
    }
    const int64_t dist_sq = static_cast<int64_t>(dx_center) * dx_center + static_cast<int64_t>(dy_center) * dy_center;
    return dist_sq <= static_cast<int64_t>(TAP_ZONE_RADIUS) * TAP_ZONE_RADIUS;
}

/* Задержка перед показом дуги отсчета тишины (защита от мигания на паузах между словами) */
inline uint32_t compute_vad_arc_gap_ms(uint32_t silence_limit_ms)
{
    if (silence_limit_ms == 0) {
        return 0;
    }
    const uint32_t half = silence_limit_ms / 2;
    return (half < VAD_ARC_GAP_MS) ? half : VAD_ARC_GAP_MS;
}

/* Вычисляет угол оставшейся тишины для VAD Arc (360° -> 0° после преодоления гепа) */
inline uint16_t compute_vad_arc_angle(bool speech_detected, uint32_t silence_ms, uint32_t silence_limit_ms, bool* out_visible)
{
    const uint32_t gap_ms = compute_vad_arc_gap_ms(silence_limit_ms);
    if (!speech_detected || silence_limit_ms == 0 || gap_ms >= silence_limit_ms || silence_ms < gap_ms) {
        if (out_visible) *out_visible = false;
        return 0;
    }
    if (out_visible) *out_visible = true;
    const uint32_t span = silence_limit_ms - gap_ms;
    const uint32_t elapsed = silence_ms - gap_ms;
    const uint32_t remaining = (elapsed >= span) ? 0 : (span - elapsed);
    return static_cast<uint16_t>((static_cast<uint64_t>(remaining) * 360ULL) / span);
}

} // namespace kouki::ui
