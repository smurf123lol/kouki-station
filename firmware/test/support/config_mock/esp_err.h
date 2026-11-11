#ifndef CONFIG_MOCK_ESP_ERR_H
#define CONFIG_MOCK_ESP_ERR_H

/*
 * esp_err.h для стенда конфигурации: базовые коды из общего support/esp_err.h
 * плюс коды NVS и ESP_ERROR_CHECK_WITHOUT_ABORT, которые модуль использует.
 *
 * Семантика ESP_ERROR_CHECK_WITHOUT_ABORT та же, что в IDF: выражение
 * вычисляется, при ошибке программа НЕ прерывается (в образе с NDEBUG —
 * просто вычисление).
 */
#include "../esp_err.h"

#define ESP_ERR_NVS_BASE             0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED  (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND        (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH    (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY        (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_INVALID_LENGTH   (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES    (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_KEY_TOO_LONG     (ESP_ERR_NVS_BASE + 0x09)
#define ESP_ERR_NVS_INVALID_HANDLE   (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_VALUE_TOO_LONG   (ESP_ERR_NVS_BASE + 0x0e)

#define ESP_ERROR_CHECK_WITHOUT_ABORT(x)      \
    do {                                      \
        esp_err_t err_rc_ = (x);              \
        (void)err_rc_;                        \
    } while (0)

#endif  // CONFIG_MOCK_ESP_ERR_H
