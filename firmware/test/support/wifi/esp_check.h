#pragma once

/*
 * Урезанная заглушка esp_check.h для host-стенда Wifi (T121 П7).
 *
 * ESP_RETURN_ON_ERROR ведёт себя как в IDF: вычисляет выражение, при ошибке
 * пишет в лог и возвращает код из текущей функции.
 */
#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...)                    \
    do {                                                         \
        esp_err_t err_rc_ = (x);                                 \
        if (err_rc_ != ESP_OK) {                                 \
            ESP_LOGE(tag, fmt ": %s", esp_err_to_name(err_rc_)); \
            return err_rc_;                                      \
        }                                                        \
    } while (0)
