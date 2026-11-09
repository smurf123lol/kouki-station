#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "app_voice.h"
#include "voice_types.hpp"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

namespace kouki::voice {

constexpr size_t RECORD_CHUNK = 1024;
constexpr size_t BYTES_PER_SAMPLE = 2;
constexpr size_t SAMPLE_RATE = 16000;
constexpr size_t WAV_HEADER_SIZE = 44;

struct VoiceContext {
    uint8_t *buffer = nullptr;
    size_t capacity_bytes = 0;
    size_t sample_bytes = 0;

    kouki_session_t session = KOUKI_SESSION_TASK;
    kouki_voice_result_t result{};

    kouki_voice_event_cb_t listener_cb = nullptr;
    void *listener_ctx = nullptr;

    bool is_wake_recording = false;
    bool speech_detected = false;
    uint32_t silence_chunks = 0;
    bool stop_requested = false;
    bool cancel_requested = false;
    bool dialogue_continue = false;

    /* Предзапись, которую предыдущее состояние (follow-up) отдаёт записи. */
    const int16_t *pending_preroll = nullptr;
    size_t pending_preroll_samples = 0;

    uint32_t followup_silence_chunks = 0;
    uint32_t last_vad_silence_ms = 0;
    uint32_t followup_streak = 0;

    void reset_recording() noexcept {
        sample_bytes = 0;
        speech_detected = false;
        silence_chunks = 0;
        followup_silence_chunks = 0;
        stop_requested = false;
        cancel_requested = false;
        last_vad_silence_ms = 0;
    }

    void write_wav_header() noexcept {
        if (!buffer) return;
        uint32_t sb = static_cast<uint32_t>(sample_bytes);
        std::memcpy(buffer + 0, "RIFF", 4);
        put_le32(buffer + 4, 36 + sb);
        std::memcpy(buffer + 8, "WAVE", 4);
        std::memcpy(buffer + 12, "fmt ", 4);
        put_le32(buffer + 16, 16);
        put_le16(buffer + 20, 1); // PCM
        put_le16(buffer + 22, 1); // Mono
        put_le32(buffer + 24, SAMPLE_RATE);
        put_le32(buffer + 28, SAMPLE_RATE * BYTES_PER_SAMPLE);
        put_le16(buffer + 32, BYTES_PER_SAMPLE);
        put_le16(buffer + 34, 16);
        std::memcpy(buffer + 36, "data", 4);
        put_le32(buffer + 40, sb);
    }

    bool is_loud_enough(uint32_t min_amplitude) const noexcept {
        const size_t samples = sample_bytes / BYTES_PER_SAMPLE;
        if (samples == 0 || !buffer) return false;

        const int16_t *pcm = reinterpret_cast<const int16_t *>(buffer + WAV_HEADER_SIZE);
        uint64_t sum = 0;
        size_t checked = 0;
        for (size_t i = 0; i < samples; i += 4) {
            int32_t val = pcm[i];
            sum += static_cast<uint64_t>(val < 0 ? -val : val);
            checked++;
        }
        uint64_t mean = checked ? (sum / checked) : 0;
        return mean >= min_amplitude;
    }

    static bool is_chunk_loud(const int16_t *pcm, size_t samples, uint32_t threshold) noexcept {
        if (!pcm || samples == 0) return false;
        uint64_t sum = 0;
        size_t checked = 0;
        for (size_t i = 0; i < samples; i += 4) {
            int32_t val = pcm[i];
            sum += static_cast<uint64_t>(val < 0 ? -val : val);
            checked++;
        }
        return checked ? ((sum / checked) >= threshold) : false;
    }

private:
    static void put_le16(uint8_t *p, uint16_t v) noexcept {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
    static void put_le32(uint8_t *p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
};

} // namespace kouki::voice
