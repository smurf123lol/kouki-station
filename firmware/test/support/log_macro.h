#ifndef KOUKI_TEST_LOG_MACRO_H
#define KOUKI_TEST_LOG_MACRO_H

/*
 * Единые макросы ESP_LOG* для host-стендов (T121 П14).
 *
 * Все заглушки esp_log.h в support/<стенд>/ включают этот файл, чтобы набор
 * макросов был один. По умолчанию логи выключены (`if (0)`): вызовы
 * компилируются, но тексты никуда не идут. С -DKOUKI_TEST_LOG_CAPTURE те же
 * макросы складывают отформатированную строку в буфер log_capture.c, и стенд
 * может проверить ПРИЧИНУ ветки (код 401, отказ inactive_time, ошибку NVS),
 * а не только её наличие.
 */
#include <stdio.h>

#include "log_capture.h"

#ifdef KOUKI_TEST_LOG_CAPTURE
#define KOUKI_TEST_LOG_EMIT(level, tag, fmt, ...) \
    kouki_test_log_emit((level), (tag), (fmt), ##__VA_ARGS__)
#else
#define KOUKI_TEST_LOG_EMIT(level, tag, fmt, ...)                          \
    do {                                                                   \
        if (0) {                                                           \
            printf("%c %s: " fmt "\n", (level), (tag), ##__VA_ARGS__);     \
        }                                                                  \
    } while (0)
#endif

#define ESP_LOGE(tag, fmt, ...) KOUKI_TEST_LOG_EMIT('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) KOUKI_TEST_LOG_EMIT('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) KOUKI_TEST_LOG_EMIT('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) KOUKI_TEST_LOG_EMIT('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) KOUKI_TEST_LOG_EMIT('V', tag, fmt, ##__VA_ARGS__)

#endif  // KOUKI_TEST_LOG_MACRO_H
