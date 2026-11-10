#pragma once

#include <stdint.h>

/* Урезанный FreeRTOS для host-стенда T121 П5: только типы, которые нужны
 * esp_board_init.h и app_audio_out.h вне ESP_PLATFORM. */
typedef uint32_t TickType_t;
typedef int BaseType_t;

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFFu
#endif
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
