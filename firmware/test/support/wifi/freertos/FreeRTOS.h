#pragma once

/*
 * Урезанный FreeRTOS для host-стенда Wifi (T121 П7): только типы и макросы,
 * которые нужны app_wifi.cpp вне ESP_PLATFORM.
 */
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFFu
#endif
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
