#ifndef APP_HISTORY_H
#define APP_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KOUKI_HISTORY_CAPACITY 10

typedef struct {
    char transcript[192];
    char title[128];
    char answer[512];
    char outcome[24];
    char timestamp_hm[8]; // "18:05"
    bool is_task;
    bool is_answer;
} kouki_history_item_t;

/**
 * @brief Выделяет буфер истории в PSRAM (или куче) и инициализирует мьютекс.
 */
void kouki_history_init(void);

/**
 * @brief Добавляет результат распознавания/ответа в кольцевой буфер.
 * Если буфер полон (10 записей), вытесняет самую старую запись.
 */
void kouki_history_push(const kouki_voice_result_t *res);

/**
 * @brief Возвращает текущее количество записей в истории (от 0 до KOUKI_HISTORY_CAPACITY).
 */
size_t kouki_history_count(void);

/**
 * @brief Возвращает монотонный счётчик изменений истории (для O(1)-проверки в UI без копирования элемента).
 */
uint32_t kouki_history_version(void);

/**
 * @brief Извлекает запись по индексу:
 * index = 0 — самая свежая запись, index = 1 — предыдущая, и т.д.
 * 
 * @param index Индекс записи (0 .. count - 1).
 * @param out_item Указатель на структуру для копирования.
 * @return true если запись успешно скопирована, false если индекс вне диапазона.
 */
bool kouki_history_get(size_t index, kouki_history_item_t *out_item);

/**
 * @brief Очищает историю.
 */
void kouki_history_clear(void);

#ifdef __cplusplus
}
#endif

#endif // APP_HISTORY_H
