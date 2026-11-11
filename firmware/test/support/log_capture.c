/*
 * Реализация перехвата ESP_LOG* для host-стендов (T121 П14).
 *
 * Собирается отдельным объектом и линкуется только в те стенды, которые
 * собираются с -DKOUKI_TEST_LOG_CAPTURE: без этого флага заглушки esp_log.h
 * остаются пустыми (if (0)), и объект не нужен.
 */
#include "log_capture.h"

#ifdef KOUKI_TEST_LOG_CAPTURE

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_CAPACITY 8192

static char g_text[LOG_CAPACITY];
static size_t g_len = 0;
static int g_lines = 0;

void kouki_test_log_clear(void)
{
    g_len = 0;
    g_lines = 0;
    g_text[0] = '\0';
}

void kouki_test_log_emit(char level, const char *tag, const char *fmt, ...)
{
    char line[1024];
    int n = snprintf(line, sizeof(line), "%c %s: ", level, tag != NULL ? tag : "?");
    if (n < 0) {
        return;
    }
    size_t used = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;

    va_list args;
    va_start(args, fmt);
    const int m = vsnprintf(line + used, sizeof(line) - used, fmt, args);
    va_end(args);
    if (m > 0) {
        used += (size_t)m < sizeof(line) - used ? (size_t)m : sizeof(line) - used - 1;
    }
    if (used + 1 < sizeof(line)) {
        line[used++] = '\n';
        line[used] = '\0';
    }

    /* Кольцо на весь буфер: старое вытесняется, чтобы длинный прогон не рос.
     * Одна строка не длиннее sizeof(line) = 1024 < LOG_CAPACITY, поэтому
     * достаточно вытеснить недостающий кусок спереди. */
    if (g_len + used >= LOG_CAPACITY) {
        const size_t drop = g_len + used - LOG_CAPACITY + 1;
        memmove(g_text, g_text + drop, g_len - drop);
        g_len -= drop;
    }
    memcpy(g_text + g_len, line, used);
    g_len += used;
    g_text[g_len] = '\0';
    g_lines++;
}

const char *kouki_test_log_text(void)
{
    return g_text;
}

int kouki_test_log_count(void)
{
    return g_lines;
}

int kouki_test_log_contains(const char *needle)
{
    return (needle != NULL && strstr(g_text, needle) != NULL) ? 1 : 0;
}

#endif  // KOUKI_TEST_LOG_CAPTURE
