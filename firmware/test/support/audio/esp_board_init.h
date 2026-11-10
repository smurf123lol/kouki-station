#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Кодек станции (firmware/components/audio_board). На стенде — мок в audio_mock.c. */
esp_err_t esp_audio_play(const int16_t *data, int length, TickType_t ticks_to_wait);
esp_err_t esp_audio_get_play_vol(int *volume);
esp_err_t esp_audio_set_play_vol(int volume);

#ifdef __cplusplus
}
#endif
