#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Поднимает детектор кодового слова.
 *
 * Модели лежат в flash-партиции (CONFIG_KOUKI_WAKE_PARTITION), поэтому без
 * раздела `model` в partitions.csv детектор молча не включится: в логе будет
 * строка о том, что модель не найдена.
 *
 * @return ESP_OK, если детектор готов; ESP_ERR_NOT_FOUND — нет партиции или модели.
 */
esp_err_t kouki_wake_init(void);

/** @brief Освобождает модель и таблицу разделов. Повторный init после этого разрешён. */
void kouki_wake_deinit(void);

/** @brief true, если детектор готов принимать звук. */
bool kouki_wake_is_ready(void);

/** @brief Имя модели, которую реально загрузили (для лога и дашборда). */
const char *kouki_wake_model(void);

/**
 * @brief Проверяет кусок микрофонного сигнала (16 кГц, моно).
 *
 * Функция не блокирует и не выделяет память: неполный чанк складывается во
 * внутренний буфер, поэтому куски могут быть любого размера.
 *
 * @return true, если в этом куске услышали кодовое слово.
 */
bool kouki_wake_feed(const int16_t *pcm, int samples);

#ifdef __cplusplus
}
#endif
