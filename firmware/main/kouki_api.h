/*
 * Точка стыка C-кода (`main.c`) с объектным графом станции (T121 П13).
 *
 * `main.c` остаётся C-файлом (спека §5), поэтому ему нужен C-заголовок:
 * сборка объектов и порядок инициализации задаются здесь, а не в main.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Собирает объекты станции (kouki::station()) до первого обращения к модулю. */
esp_err_t kouki_station_wire(void);

/**
 * Прежний порядок инициализации `app_main` (спека §1): audio_board -> audio_out
 * -> player -> EXIO -> LCD -> LVGL -> voice -> history -> power -> ui -> консоль
 * -> ota -> Wi-Fi/SoftAP. Возвращает первый esp_err_t отказа.
 */
esp_err_t kouki_station_init(void);

#ifdef __cplusplus
}
#endif
