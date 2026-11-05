/*
 * Модуль OTA-обновлений прошивки станции Kouki.
 *
 * Опрашивает сервер GET /api/firmware?current=<version>, скачивает образ
 * по https через esp_https_ota с pinned CA, проверяет sha256 и размер,
 * ставит в свободный OTA-слот в состоянии покоя (IDLE) и перезагружает плату.
 *
 * Новый образ стартует в состоянии PENDING_VERIFY; при первом успешном
 * HTTP(S) обмене с сервером вызывается esp_ota_mark_app_valid_cancel_rollback().
 */
#ifndef APP_OTA_H
#define APP_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Инициализирует модуль OTA: проверяет состояние отката bootloader,
 * создает задачу периодического опроса обновлений.
 */
esp_err_t kouki_ota_init(void);

/**
 * Запускает опрос обновлений (вызывать после успешного подключения к сети).
 */
void kouki_ota_start(void);

/**
 * Уведомляет модуль OTA об успешном обмене с сервером (HTTP/HTTPS).
 * Если текущий образ запущен в режиме PENDING_VERIFY, подтверждает
 * валидность образа вызовом esp_ota_mark_app_valid_cancel_rollback().
 */
void kouki_ota_mark_server_communicated(void);

/**
 * Проверяет, идёт ли сейчас процесс загрузки или записи OTA.
 */
bool kouki_ota_is_updating(void);

/**
 * Сравнивает две строки semver (например, "0.1.0" и "0.1.1").
 * Возвращает:
 *  > 0, если v1 > v2
 *  0, если v1 == v2
 *  < 0, если v1 < v2
 */
int kouki_ota_semver_compare(const char *v1, const char *v2);

#ifdef __cplusplus
}
#endif

#endif // APP_OTA_H
