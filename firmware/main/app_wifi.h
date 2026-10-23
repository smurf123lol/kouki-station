/*
 * Wi-Fi в режиме станции: одна сеть из menuconfig, без сканирования и без
 * сохранения в NVS. Пароль лежит в прошивке — для домашней станции это
 * осознанный компромисс: перепрошивка дешевле, чем меню настройки на экране.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Блокируется до получения адреса или до таймаута CONFIG_KOUKI_WIFI_TIMEOUT_S. */
esp_err_t kouki_wifi_connect(void);

bool kouki_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
