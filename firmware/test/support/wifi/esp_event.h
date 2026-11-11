#pragma once

/*
 * Заглушка esp_event.h для host-стенда Wifi (T121 П7).
 *
 * Событийная шина не поднимается: регистрация обработчиков запоминается, а
 * события «приходят» из теста через wifi_mock_fire_event(). Так проверяется
 * именно логика app_wifi.cpp — переходы состояний и backoff повторов.
 */
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *esp_event_base_t;

/* «Любой идентификатор события» — как в IDF. */
#define ESP_EVENT_ANY_ID (-1)
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

typedef void *esp_event_handler_instance_t;

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t event_base, int32_t event_id,
                                              esp_event_handler_t event_handler, void *event_handler_arg,
                                              esp_event_handler_instance_t *context);

#ifdef __cplusplus
}
#endif
