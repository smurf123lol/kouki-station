/*
 * Экран станции: одна большая кнопка и текст ответа. Круглый дисплей 360×360,
 * поэтому всё выровнено по центру, а длинный текст переносится.
 *
 * Обновление идёт из LVGL-таймера, а не из задач записи и отправки: LVGL
 * не потокобезопасен, а читать снимок состояния под мьютексом дешевле, чем
 * ловить гонки между HTTP-задачей и отрисовкой.
 */
#pragma once

#include "app_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Создаёт виджеты на текущем экране LVGL (вызывать после LVGL_Init). */
void kouki_ui_init(void);

/* Показывает экран первичной настройки (AP-режим). */
void kouki_ui_show_setup(const char *ssid, const char *url);

/* Показывает экран процесса OTA обновления прошивки */
void kouki_ui_show_ota_progress(const char *text);

/* Сбрасывает экран OTA обновления (если произошла ошибка) */
void kouki_ui_clear_ota(void);

#ifdef __cplusplus
}
#endif
