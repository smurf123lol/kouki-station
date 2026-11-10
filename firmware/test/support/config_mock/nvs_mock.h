#ifndef NVS_MOCK_H
#define NVS_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Мок NVS для host-стенда (T121 П2).
 *
 * Повторяет поведение IDF там, где оно важно модулю конфигурации:
 * - nvs_open(..., NVS_READWRITE) создаёт namespace, если его нет;
 * - NVS_READONLY для отсутствующего namespace даёт ESP_ERR_NVS_NOT_FOUND;
 * - nvs_get_str при слишком коротком буфере — ESP_ERR_NVS_INVALID_LENGTH;
 * - nvs_set_* пишет в «флеш», nvs_commit фиксирует, nvs_erase_all/erase_key
 *   удаляют; данные живут до nvs_mock_reset().
 *
 * Дополнительно мок ведёт журнал записей (nvs_mock_was_written), чтобы стенд
 * проверял миграцию wake_mig: какие именно ключи модуль перезаписал.
 */

/** Полная очистка «флеша», журналов и заготовленных ошибок open. */
void nvs_mock_reset(void);

/** Следующий nvs_open() вернёт заданный код (после вызова сбрасывается). */
void nvs_mock_fail_next_open(esp_err_t err);

/** Следующий nvs_get_str() по ключу вернёт заданный код. */
void nvs_mock_fail_next_get_str(const char *key, esp_err_t err);

/** Есть ли namespace в «флеше». */
bool nvs_mock_has_namespace(const char *ns);

/** Сколько ключей в namespace (0, если namespace нет). */
int nvs_mock_count_keys(const char *ns);

/** Есть ли ключ в namespace. */
bool nvs_mock_has_key(const char *ns, const char *key);

/** Тип и значение ключа; чтение несуществующего ключа возвращает false. */
bool nvs_mock_get_u8(const char *ns, const char *key, uint8_t *out);
bool nvs_mock_get_u32(const char *ns, const char *key, uint32_t *out);
bool nvs_mock_get_str(const char *ns, const char *key, const char **out);


/** Писала ли логика в этот ключ за текущий тест (журнал nvs_set_.../nvs_erase_...). */
bool nvs_mock_was_written(const char *ns, const char *key);

/** Всё, что было записано с момента nvs_mock_reset(): «ns:key» через запятую. */
const char *nvs_mock_write_log(void);

/** Имя namespace из последнего nvs_open(). */
const char *nvs_mock_last_open_namespace(void);

/** Прямая закладка значения в «флеш» (мимо API модуля), без записи в журнал. */
void nvs_mock_seed_u8(const char *ns, const char *key, uint8_t value);
void nvs_mock_seed_u32(const char *ns, const char *key, uint32_t value);
void nvs_mock_seed_str(const char *ns, const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif  // NVS_MOCK_H
