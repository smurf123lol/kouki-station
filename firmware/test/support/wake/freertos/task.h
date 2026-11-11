#pragma once

/*
 * Заглушка freertos/task.h для host-стенда Wake (T121 П9).
 *
 * app_wake_verify.cpp создаёт задачу ASR-проверки через xTaskCreate(); в стенде
 * она не запускается сама — тест прогоняет её через wake_mock_run_task() в
 * текущем потоке. Так проверяются и успешный путь (POST ушёл, ответ разобран),
 * и ошибочные ветки, без реальной многопоточности.
 */
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t task);

/** Заказать сбой следующего xTaskCreate (pdPASS — успех). */
void wake_mock_fail_next_task_create(void);

/** Сколько задач создано за тест. */
int wake_mock_task_creates(void);

/**
 * Прогнать последнюю созданную задачу в текущем потоке: вызвать её функцию
 * с сохранённым аргументом и обнулить дескриптор (задача завершилась).
 *
 * @return false, если запускать нечего.
 */
bool wake_mock_run_task(void);

/** Стек и приоритет последней созданной задачи (для проверки, что не меняли). */
uint32_t wake_mock_last_task_stack(void);
unsigned wake_mock_last_task_priority(void);
const char *wake_mock_last_task_name(void);

#ifdef __cplusplus
}
#endif
