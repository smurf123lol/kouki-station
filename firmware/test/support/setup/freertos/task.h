#pragma once

/*
 * Заглушка freertos/task.h для host-стенда Setup (T121 П11).
 *
 * xTaskCreate запоминает функцию и аргумент, vTaskDelay двигает мок-часы,
 * vTaskDelete помечает задачу завершённой. Консольная задача в стенде не
 * читает stdin: тест зовёт handle_console_command() напрямую.
 */
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);

/** Заказать сбой следующего xTaskCreate. */
void setup_task_fail_next_create(void);

/** Сколько задач создано; стек/приоритет/имя последней. */
int setup_task_creates(void);
uint32_t setup_task_last_stack(void);
unsigned setup_task_last_priority(void);
const char *setup_task_last_name(void);

/** Прогнать последнюю созданную задачу в текущем потоке (once). */
bool setup_task_run_last(void);

/** Сколько тиков суммарно отдали в vTaskDelay и сколько раз его звали. */
uint64_t setup_task_delay_ticks(void);
int setup_task_delay_calls(void);

/** Сколько раз задачу удаляли через vTaskDelete. */
int setup_task_deletes(void);

#ifdef __cplusplus
}
#endif
