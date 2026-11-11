#pragma once

/*
 * Заглушка event_groups.h для host-стенда Wifi (T121 П7).
 *
 * Группа событий — просто битовая маска: xEventGroupWaitBits возвращает её
 * без ожидания, потому что в стенде события приходят синхронно из теста.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t EventBits_t;

#define BIT0 (1u << 0)
#define BIT1 (1u << 1)

typedef struct event_group_mock *EventGroupHandle_t;

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
