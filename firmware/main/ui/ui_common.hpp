#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef ESP_PLATFORM
#include "lvgl.h"
#else
// For host-check stands
#include "lvgl_mock.hpp"
#endif

LV_FONT_DECLARE(font_ru_14);
LV_FONT_DECLARE(font_ru_16);
LV_FONT_DECLARE(font_ru_20);

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

} // namespace kouki::ui
