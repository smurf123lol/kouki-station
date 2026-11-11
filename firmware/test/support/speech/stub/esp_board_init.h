#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_audio_play(const int16_t *data, int length, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
