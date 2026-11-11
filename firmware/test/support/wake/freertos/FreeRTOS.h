#pragma once

/*
 * Урезанный FreeRTOS для host-стенда Wake (T121 П9).
 *
 * База (TickType_t, pdPASS, portMAX_DELAY, pdMS_TO_TICKS) берётся из стенда
 * Wifi: один набор типов обслуживает все стенды проекта. Здесь добавляется
 * только то, чего у Wifi нет, но что нужно app_wake.cpp и
 * app_wake_verify.cpp: мьютекс состояния, критическая секция для ленивого
 * создания мьютекса и дескриптор задачи.
 *
 * Стенд однопоточный: семафор — счётчик занятости, критическая секция —
 * пустые макросы, задача не запускается сама (её прогоняет тест через
 * wake_mock_run_task()). Порядок «взял — отпустил» от этого не меняется, а
 * гонок в одном потоке нет по построению.
 */
#include "../../wifi/freertos/FreeRTOS.h"

/* Handle мьютекса в IDF — указатель на структуру очереди. */
typedef struct wake_sem_mock *SemaphoreHandle_t;

/* Handle задачи в IDF — указатель на структуру TCB. */
typedef struct wake_task_mock *TaskHandle_t;

/* Приоритет простаивающей задачи: в тесте очередь выполнения не работает. */
#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif

/* Тип спинлока для portMUX. На хосте блокировка не нужна. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0

#define taskENTER_CRITICAL(lock) do { (void)(lock); } while (0)
#define taskEXIT_CRITICAL(lock) do { (void)(lock); } while (0)
