#ifndef APP_SETUP_H
#define APP_SETUP_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Запускает фоновую задачу USB/Serial консоли (чтение stdin).
 * Команды: show, set_wifi, set_token, set_url, set_source, save, reset, reboot, help.
 */
esp_err_t kouki_setup_console_init(void);

/**
 * Запускает режим SoftAP настройки:
 * - Поднимает SoftAP «Kouki-Setup-XXXX» (XXXX — 2 байта MAC)
 * - Поднимает HTTP сервер на 192.168.4.1:80
 * - Переключает экран в режим «Не настроено»
 */
esp_err_t kouki_setup_start(void);

/**
 * Проверяет, активен ли режим настройки.
 */
bool kouki_setup_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // APP_SETUP_H
