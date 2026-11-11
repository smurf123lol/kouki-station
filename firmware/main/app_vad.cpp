#include "app_vad.hpp"

#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace kouki {
namespace {

#ifdef ESP_PLATFORM
const char *const TAG = "kouki.vad";
#endif

}  // namespace

esp_err_t Vad::init()
{
    if (m_initialized) {
        return ESP_OK;
    }

#ifdef ESP_PLATFORM
    m_handle = vad_create(VAD_MODE_3);
    if (!m_handle) {
        ESP_LOGE(TAG, "Не удалось инициализировать WebRTC VAD (VAD_MODE_3)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WebRTC VAD инициализирован: mode 3, 16000 Гц, фреймы 30 мс (480 сэмплов)");
#endif

    m_initialized = true;
    reset();
    return ESP_OK;
}

void Vad::deinit()
{
    if (!m_initialized) {
        return;
    }

#ifdef ESP_PLATFORM
    if (m_handle) {
        vad_destroy(m_handle);
        m_handle = nullptr;
    }
#endif

    m_initialized = false;
    m_fifo_len = 0;
    m_speech_streak = 0;
    m_last_frame_speech = false;
}

void Vad::reset()
{
    m_fifo_len = 0;
    m_speech_streak = 0;
    m_last_frame_speech = false;

#ifdef ESP_PLATFORM
    if (m_handle) {
        vad_reset_trigger(m_handle);
    }
#endif
}

bool Vad::is_active()
{
    return m_initialized;
}

uint32_t Vad::speech_streak()
{
    return m_speech_streak;
}

bool Vad::is_speech_streak(uint32_t min_frames)
{
    return (m_speech_streak >= min_frames);
}

bool Vad::process(const int16_t *pcm, size_t samples)
{
    if (!m_initialized) {
        init();
    }
    if (!pcm || samples == 0) {
        return false;
    }

    bool speech_in_chunk = false;
    size_t offset = 0;

    while (offset < samples) {
        size_t needed = kFrameSamples - m_fifo_len;
        size_t to_copy = (samples - offset < needed) ? (samples - offset) : needed;
        memcpy(m_fifo + m_fifo_len, pcm + offset, to_copy * sizeof(int16_t));
        m_fifo_len += to_copy;
        offset += to_copy;

        if (m_fifo_len == kFrameSamples) {
            bool frame_speech = false;
#ifdef ESP_PLATFORM
            if (m_handle) {
                vad_state_t st = vad_process(m_handle, m_fifo, 16000, 30);
                frame_speech = (st == VAD_SPEECH);
            }
#else
            frame_speech = host_evaluate(m_fifo, kFrameSamples);
#endif
            if (frame_speech) {
                m_speech_streak++;
                speech_in_chunk = true;
            } else {
                m_speech_streak = 0;
            }
            m_last_frame_speech = frame_speech;
            m_fifo_len = 0;
        }
    }

    return speech_in_chunk || (m_speech_streak > 0);
}

#ifndef ESP_PLATFORM
bool Vad::host_evaluate(const int16_t *pcm, size_t samples)
{
    if (m_host_mock_enabled) {
        return m_host_mock_speech;
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

void Vad::host_set_mock(bool enabled, bool speech_result)
{
    m_host_mock_enabled = enabled;
    m_host_mock_speech = speech_result;
}
#endif

Vad &vad()
{
    static Vad s_instance;
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

esp_err_t kouki_vad_init(void)
{
    return kouki::vad().init();
}

void kouki_vad_deinit(void)
{
    kouki::vad().deinit();
}

bool kouki_vad_process(const int16_t *pcm, size_t samples)
{
    return kouki::vad().process(pcm, samples);
}

uint32_t kouki_vad_get_speech_streak(void)
{
    return kouki::vad().speech_streak();
}

bool kouki_vad_is_speech_streak(uint32_t min_frames)
{
    return kouki::vad().is_speech_streak(min_frames);
}

void kouki_vad_reset(void)
{
    kouki::vad().reset();
}

bool kouki_vad_is_active(void)
{
    return kouki::vad().is_active();
}

#ifndef ESP_PLATFORM
void kouki_vad_host_set_mock(bool enabled, bool speech_result)
{
    kouki::vad().host_set_mock(enabled, speech_result);
}
#endif

}  // extern "C"
