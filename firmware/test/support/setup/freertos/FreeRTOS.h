#pragma once

/*
 * Урезанный FreeRTOS для host-стенда Setup (T121 П11).
 *
 * База (TickType_t, pdPASS, pdMS_TO_TICKS) берётся из стенда Wifi: один набор
 * типов обслуживает все стенды проекта. Здесь добавляется дескриптор задачи:
 * app_setup.cpp создаёт задачи консоли и отложенной перезагрузки, стенд
 * прогоняет их по команде теста в текущем потоке.
 */
#include "../../wifi/freertos/FreeRTOS.h"

/* Handle задачи в IDF — указатель на структуру TCB. */
typedef struct setup_task_mock *TaskHandle_t;
