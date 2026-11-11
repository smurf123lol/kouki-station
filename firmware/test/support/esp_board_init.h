#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_audio_get_play_vol(int *volume);
esp_err_t esp_audio_set_play_vol(int volume);

#ifdef __cplusplus
}
#endif
