#include "app_vad.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_vad.h"
#include "esp_log.h"
static const char *TAG = "kouki.vad";
static vad_handle_t s_vad_handle = NULL;
#else
static bool s_host_mock_enabled = false;
static bool s_host_mock_speech = false;
#endif

#define VAD_FRAME_SAMPLES 480 // 30 мс при 16 кГц

static bool s_vad_initialized = false;
static int16_t s_vad_fifo[VAD_FRAME_SAMPLES];
static size_t s_vad_fifo_len = 0;
static uint32_t s_speech_streak = 0;
static bool s_last_frame_speech = false;

#ifndef ESP_PLATFORM
static bool host_vad_evaluate(const int16_t *pcm, size_t samples)
{
    if (s_host_mock_enabled) {
        return s_host_mock_speech;
    }
    if (!pcm || samples == 0) return false;

    uint64_t sum = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        sum += (v < 0 ? -v : v);
    }
    uint64_t mean = sum / samples;
    return (mean >= 300);
}
#endif

esp_err_t kouki_vad_init(void)
{
    if (s_vad_initialized) {
        return ESP_OK;
    }

#ifdef ESP_PLATFORM
    s_vad_handle = vad_create(VAD_MODE_3);
    if (!s_vad_handle) {
        ESP_LOGE(TAG, "Не удалось инициализировать WebRTC VAD (VAD_MODE_3)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WebRTC VAD инициализирован: mode 3, 16000 Гц, фреймы 30 мс (480 сэмплов)");
#endif

    s_vad_initialized = true;
    kouki_vad_reset();
    return ESP_OK;
}

void kouki_vad_deinit(void)
{
    if (!s_vad_initialized) {
        return;
    }

#ifdef ESP_PLATFORM
    if (s_vad_handle) {
        vad_destroy(s_vad_handle);
        s_vad_handle = NULL;
    }
#endif

    s_vad_initialized = false;
    s_vad_fifo_len = 0;
    s_speech_streak = 0;
    s_last_frame_speech = false;
}

void kouki_vad_reset(void)
{
    s_vad_fifo_len = 0;
    s_speech_streak = 0;
    s_last_frame_speech = false;

#ifdef ESP_PLATFORM
    if (s_vad_handle) {
        vad_reset_trigger(s_vad_handle);
    }
#endif
}

bool kouki_vad_is_active(void)
{
    return s_vad_initialized;
}

uint32_t kouki_vad_get_speech_streak(void)
{
    return s_speech_streak;
}

bool kouki_vad_is_speech_streak(uint32_t min_frames)
{
    return (s_speech_streak >= min_frames);
}

bool kouki_vad_process(const int16_t *pcm, size_t samples)
{
    if (!s_vad_initialized) {
        kouki_vad_init();
    }
    if (!pcm || samples == 0) {
        return false;
    }

    bool speech_in_chunk = false;
    size_t offset = 0;

    while (offset < samples) {
        size_t needed = VAD_FRAME_SAMPLES - s_vad_fifo_len;
        size_t to_copy = (samples - offset < needed) ? (samples - offset) : needed;
        memcpy(s_vad_fifo + s_vad_fifo_len, pcm + offset, to_copy * sizeof(int16_t));
        s_vad_fifo_len += to_copy;
        offset += to_copy;

        if (s_vad_fifo_len == VAD_FRAME_SAMPLES) {
            bool frame_speech = false;
#ifdef ESP_PLATFORM
            if (s_vad_handle) {
                vad_state_t st = vad_process(s_vad_handle, s_vad_fifo, 16000, 30);
                frame_speech = (st == VAD_SPEECH);
            }
#else
            frame_speech = host_vad_evaluate(s_vad_fifo, VAD_FRAME_SAMPLES);
#endif
            if (frame_speech) {
                s_speech_streak++;
                speech_in_chunk = true;
            } else {
                s_speech_streak = 0;
            }
            s_last_frame_speech = frame_speech;
            s_vad_fifo_len = 0;
        }
    }

    return speech_in_chunk || (s_speech_streak > 0);
}

#ifndef ESP_PLATFORM
void kouki_vad_host_set_mock(bool enabled, bool speech_result)
{
    s_host_mock_enabled = enabled;
    s_host_mock_speech = speech_result;
}
#endif
