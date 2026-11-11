#pragma once

/*
 * Мок окружения для host-стенда Wake (T121 П9).
 *
 * Заменяет то, чего на хосте нет: мьютекс/критическую секцию FreeRTOS, задачу
 * (её прогоняет тест), esp-dl rfft (честный DFT с тем же форматом выхода),
 * heap_caps_malloc (обычный malloc) и монотонное время esp_timer_get_time
 * (переменная, которую двигает тест).
 *
 * Заголовки по отдельности лежат рядом (freertos/FreeRTOS.h, freertos/semphr.h,
 * freertos/task.h, dl_rfft.h, esp_heap_caps.h, esp_timer.h) — модуль включает
 * их ровно так же, как в IDF. Здесь — только сброс и общие счётчики, чтобы тесту
 * не тянуть пять файлов ради одной функции.
 */
#include <stdbool.h>
#include <stdint.h>

#include "dl_rfft.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Полный сброс моков: счётчики, «часы», заказанные сбои, созданные задачи. */
void wake_mock_reset(void);

#ifdef __cplusplus
}
#endif
