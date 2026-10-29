#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Озвучивает ответ сервера: POST /api/speech, WAV в ответ, воспроизведение через BSP.
 *
 * Функция синхронная: возвращает управление, когда проигрывание закончилось
 * (или сразу, если сервер не смог синтезировать речь). Вызывать из задачи,
 * которой не страшно подождать пару секунд — например, из задачи отправки.
 *
 * @param text Текст ответа; пустая строка — сразу ESP_OK.
 * @return ESP_OK, если воспроизведение прошло, иначе код ошибки.
 */
esp_err_t kouki_speech_play(const char *text);

#ifdef __cplusplus
}
#endif
