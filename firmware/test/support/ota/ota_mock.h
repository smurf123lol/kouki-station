#pragma once

/*
 * Управление моком OTA-стенда (T121 П8).
 *
 * Мок закрывает IDF-поверхность app_ota.cpp, которой нет в других стендах:
 * разделы и состояние образа (esp_ota_ops), HTTPS-OTA (esp_https_ota), задачу
 * с «часами» (freertos) и перезагрузку (esp_system). HTTP-клиент проверки
 * обновлений берётся общий — support/http_mock/ (он уже умеет отдать статус
 * и тело GET-ответа, см. http_mock_set_response).
 *
 * Через мок задаётся всё, что в образе приходит извне: состояние раздела,
 * байты образа и код ошибки на любом шаге установки. Тест видит настоящие
 * ветки app_ota.cpp — включая подсчёт sha256 по скачанным данным.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Полный сброс: слот VALID, счётчики, «часы» и очереди вызовов. */
void ota_mock_reset(void);

/* ------------------------------------------------------- раздел и образ -- */

/** Заказать состояние активного раздела (ESP_OTA_IMG_VALID и т. п.). */
void ota_mock_set_image_state(int state);

/** Метка активного раздела, которую вернёт esp_ota_get_running_partition. */
void ota_mock_set_running_label(const char *label);

/** Сколько раз подтверждали образ (esp_ota_mark_app_valid_cancel_rollback). */
int ota_mock_mark_valid_calls(void);

/** Следующий вызов подтверждения вернёт заданный код (ESP_OK — успех). */
void ota_mock_fail_mark_valid(int err);

/* ------------------------------------------------------------ https_ota -- */

/** URL, с которым пришёл последний esp_https_ota_begin (NULL — не звали). */
const char *ota_mock_last_url(void);

/** Значение заголовка Authorization, который выставил колбэк инициализации. */
const char *ota_mock_auth_header(void);

/** Сколько раз вызывали http_client_init_cb и с каким результатом. */
int ota_mock_init_cb_calls(void);
int ota_mock_init_cb_result(void);

/**
 * Очередь ответов esp_https_ota_perform: сколько раз вернуть
 * ESP_ERR_HTTPS_OTA_IN_PROGRESS, затем финальный код (ESP_OK — данные
 * получены). Байты образа доставляются событием ON_DATA на первом вызове.
 */
void ota_mock_set_perform(int in_progress_rounds, int final_code);

/** Следующий esp_https_ota_begin вернёт заданный код (ESP_OK — успех). */
void ota_mock_fail_begin(int err);

/** Следующий esp_https_ota_finish вернёт заданный код. */
void ota_mock_fail_finish(int err);

/** Считать данные полученными не полностью. */
void ota_mock_set_complete(bool complete);

/** Байты образа, которые «скачиваются» в esp_https_ota_perform. */
void ota_mock_set_image(const void *data, size_t len);

/** Счётчики вызовов: порядок begin -> perform -> finish/abort. */
int ota_mock_begin_calls(void);
int ota_mock_perform_calls(void);
int ota_mock_finish_calls(void);
int ota_mock_abort_calls(void);

/** Поля конфигурации, с которыми шли в esp_https_ota_begin (NULL — не звали). */
int ota_mock_http_timeout_ms(void);
bool ota_mock_keep_alive(void);

/* ------------------------------------------------ задача, часы, перезагрузка -- */

/** Сколько раз создавали задачу OTA; стек и приоритет последней. */
int ota_mock_task_create_calls(void);
int ota_mock_task_stack(void);
int ota_mock_task_priority(void);

/** Следующий xTaskCreate вернёт заданный код (pdPASS — успех, 0 — ошибка). */
void ota_mock_fail_task_create(int ret);

/** Сдвинуть «часы» вперёд (xTaskGetTickCount вернёт эту величину, в мс). */
void ota_mock_advance_ticks(uint32_t ms);

/** Текущее значение «часов», мс. */
uint32_t ota_mock_ticks(void);

/** Сколько раз задача уходила в задержку и сколько всего «проспала». */
int ota_mock_delay_calls(void);
uint32_t ota_mock_delay_total_ms(void);

/** Сколько раз просили перезагрузку платы. */
int ota_mock_restart_calls(void);


#ifdef __cplusplus
}
#endif
