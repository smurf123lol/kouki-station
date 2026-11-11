#pragma once

/*
 * Заглушка esp_timer.h для host-стенда Wifi (T121 П7).
 *
 * Одноразовый таймер повтора не запускается по-настоящему: вызов
 * запоминается (интервал и число запусков), а «срабатывание» тест делает сам
 * через wifi_mock_fire_retry_timer().
 */
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_cb_t callback;
    const char *name;
    void *arg;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);

#ifdef __cplusplus
}
#endif
