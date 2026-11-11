#pragma once

/*
 * Заглушка esp_heap_caps.h для host-стенда Wake (T121 П9).
 *
 * На хосте PSRAM нет: все флаги — теги, которые ни на что не влияют.
 * heap_caps_malloc — обычный malloc, как и в STUB esp_heap_caps.h из IDF-сборки
 * на хосте. Задача стенда — проверить порядок выделений и освобождений в
 * init()/deinit(), а не раскладку по банкам памяти.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_SPIRAM 2u
#define MALLOC_CAP_8BIT 4u

/* extern "C" обязателен: мок собран gcc как C, а зовёт его C++-код модуля. */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Выделяет память (caps игнорируются). Отдельная функция, а не макрос, чтобы
 * стенд мог заказывать сбой и считать запросы.
 */
void *heap_caps_malloc(size_t size, uint32_t caps);

/** Заказать сбой следующих N вызовов heap_caps_malloc. */
void wake_mock_fail_heap_caps_malloc(int times);

/** Сколько всего вызовов heap_caps_malloc было и сколько из них с PSRAM. */
int wake_mock_heap_caps_calls(void);
int wake_mock_heap_caps_spiram_calls(void);

#ifdef __cplusplus
}
#endif
