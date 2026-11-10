#pragma once

/*
 * Урезанный FreeRTOS для host-стенда Ota (T121 П8).
 *
 * Базу (TickType_t, pdMS_TO_TICKS, portMAX_DELAY) берём из стенда Wifi, чтобы
 * один набор моков обслуживал и его: стенд Ota линкует настоящий app_wifi.cpp
 * (экземпляр Wi-Fi нужен конструктору Ota). Здесь только то, чего нет у Wifi:
 * задача и «часы».
 *
 * Задача OTA в стенде по-настоящему не крутится: xTaskCreate запоминает
 * функцию и аргумент, а «часы» двигает тест через ota_mock_advance_ticks() —
 * так проверяется таймаут PENDING_VERIFY без бесконечного цикла задачи.
 */
#include "../../wifi/freertos/FreeRTOS.h"

/* Handle задачи в IDF — указатель на структуру TCB. */
typedef struct ota_task_mock *TaskHandle_t;

/* extern "C" обязателен: мок собран gcc как C, а app_ota.cpp зовёт эти
 * функции из C++ — без блока имена разошлись бы по манглингу. */
#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *out);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
void vTaskDelete(TaskHandle_t task);

#ifdef __cplusplus
}
#endif
