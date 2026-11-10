#pragma once

/*
 * Заглушка esp_https_ota.h для host-стенда Ota (T121 П8).
 *
 * Повторяет поверхность, которой пользуется app_ota.cpp: begin/perform/
 * is_complete_data_received/finish/abort и колбэк инициализации HTTP-клиента.
 * Поведение (успех, ошибка на шаге, неполный образ) задаёт тест, поэтому
 * проверяются все ветки установки без флеша и без сети.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_https_ota *esp_https_ota_handle_t;

/* Колбэк после инициализации HTTP-клиента: ставит Authorization из Config. */
typedef esp_err_t (*http_client_init_cb_t)(esp_http_client_handle_t client);

typedef struct {
    const esp_http_client_config_t *http_config;
    http_client_init_cb_t http_client_init_cb;
} esp_https_ota_config_t;

#define ESP_ERR_HTTPS_OTA_BASE (0x9000)
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS (ESP_ERR_HTTPS_OTA_BASE + 1)

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *ota_config, esp_https_ota_handle_t *handle);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle);

/* --- Управление моком из стенда --------------------------------------- */

/** Сброс: очередь ответов perform, счётчики, флаги ошибок. */
void ota_mock_reset(void);

/** Конфигурация, которую получил последний esp_https_ota_begin (NULL — не звали). */
const esp_https_ota_config_t *ota_mock_last_config(void);

/**
 * Очередь ответов esp_https_ota_perform: сколько раз вернуть IN_PROGRESS,
 * затем финальный код (ESP_OK — данные получены).
 */
void ota_mock_set_perform(int in_progress_rounds, esp_err_t final_code);

/** Следующий esp_https_ota_begin вернёт заданный код (ESP_OK — успех). */
void ota_mock_fail_begin(esp_err_t err);

/** Следующий esp_https_ota_finish вернёт заданный код. */
void ota_mock_fail_finish(esp_err_t err);

/** Считать данные полученными не полностью (esp_https_ota_is_complete_data_received). */
void ota_mock_set_complete(bool complete);

/** Сколько раз звали begin/perfom/finish/abort (для проверки порядка). */
int ota_mock_begin_calls(void);
int ota_mock_perform_calls(void);
int ota_mock_finish_calls(void);
int ota_mock_abort_calls(void);

/** Последний URL, который видел esp_https_ota_begin (http_config->url). */
const char *ota_mock_last_url(void);

#ifdef __cplusplus
}
#endif
