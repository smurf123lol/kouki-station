#include "recording_state.hpp"
#include "app_vad.h"
#include <cstring>

#ifdef ESP_PLATFORM
#include "app_tone.h"
#include "app_config.h"
#include "app_power.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace kouki::voice {

void RecordingState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    ctx.reset_recording();
    ctx.result = {};
    m_followup_speech_confirmed = false;
    kouki_vad_reset();

    if (ctx.dialogue_continue) {
        ctx.speech_detected = true;
    }

#ifdef ESP_PLATFORM
    kouki_power_notify_activity();
    m_min_amplitude = CONFIG_KOUKI_MIN_AMPLITUDE;
    m_vad_silence_ms = 3000;
    m_followup_silence_ms = CONFIG_KOUKI_FOLLOWUP_SILENCE_MS;
    m_max_record_ms = CONFIG_KOUKI_MAX_RECORD_S * 1000;
    m_record_start_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);

    if (ctx.is_wake_recording) {
        kouki_tone_play(KOUKI_TONE_WAKE);
    } else {
        kouki_tone_play(KOUKI_TONE_START);
    }
#else
    m_followup_silence_ms = 1500;
    m_record_start_ms = 0;
#endif
}

StateId RecordingState::on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) {
    if (ctx.cancel_requested) {
        return StateId::Idle;
    }
    if (ctx.stop_requested) {
        return StateId::Sending;
    }

    if (!pcm || samples == 0) {
        return StateId::Stay;
    }

    const size_t bytes = samples * BYTES_PER_SAMPLE;
    if (ctx.sample_bytes + bytes > ctx.capacity_bytes) {
        return StateId::Sending; // Буфер переполнен
    }

    if (ctx.buffer) {
        std::memcpy(ctx.buffer + WAV_HEADER_SIZE + ctx.sample_bytes, pcm, bytes);
        ctx.sample_bytes += bytes;
    }

    // Спектральная VAD логика: отличает истинную речь от шума
    const bool is_vad_speech = kouki_vad_process(pcm, samples);

    if (ctx.dialogue_continue) {
        if (is_vad_speech) {
            ctx.speech_detected = true;
            m_followup_speech_confirmed = true;
            ctx.followup_silence_chunks = 0;
            ctx.last_vad_silence_ms = 0;
        } else {
            ctx.followup_silence_chunks++;
            uint32_t silence_ms = static_cast<uint32_t>(
                ctx.followup_silence_chunks * (static_cast<uint64_t>(samples) * 1000 / SAMPLE_RATE));
            ctx.last_vad_silence_ms = silence_ms;

            uint32_t total_recorded_ms = static_cast<uint32_t>(
                (ctx.sample_bytes / BYTES_PER_SAMPLE) * 1000 / SAMPLE_RATE);

            // Если за первые 2500 мс в follow-up речь ни разу не подтвердилась VAD (только шум) —
            // отменяем запись локально без отправки на сервер и без тона ошибки
            if (!m_followup_speech_confirmed && total_recorded_ms >= 2500) {
#ifdef ESP_PLATFORM
                ESP_LOGI("kouki.fsm", "Follow-up запись отменена: VAD за 2500 мс не обнаружил речь человека");
#endif
                ctx.cancel_requested = true;
                return StateId::Idle;
            }

            if (silence_ms >= m_followup_silence_ms) {
                if (m_followup_speech_confirmed) {
                    return StateId::Sending;
                } else {
#ifdef ESP_PLATFORM
                    ESP_LOGI("kouki.fsm", "Follow-up отменен: тишина/шум без подтвержденной речи человека");
#endif
                    ctx.cancel_requested = true;
                    return StateId::Idle;
                }
            }
        }
    } else {
        // Общая логика VAD для записи по кнопке и по wake-word
        // Игнорируем первые 320 мс (хвост стартового тона)
        const size_t min_speech_start_bytes = 5 * RECORD_CHUNK * BYTES_PER_SAMPLE;
        if (ctx.sample_bytes >= min_speech_start_bytes && is_vad_speech) {
            if (!ctx.speech_detected) {
                ctx.speech_detected = true;
            }
            ctx.silence_chunks = 0;
            ctx.last_vad_silence_ms = 0;
        } else {
            ctx.silence_chunks++;
            uint32_t silence_ms = static_cast<uint32_t>(
                ctx.silence_chunks * (static_cast<uint64_t>(samples) * 1000 / SAMPLE_RATE));
            ctx.last_vad_silence_ms = silence_ms;

            if (ctx.speech_detected) {
                // Если речь была зафиксирована — через m_vad_silence_ms (3000 мс) завершаем запись
                if (silence_ms >= m_vad_silence_ms) {
                    return StateId::Sending;
                }
            } else {
                // Если речь еще не началась:
                // Для wake-сессий: через 5 с отменяем запись (ложное срабатывание)
                if (ctx.is_wake_recording && silence_ms >= 5000) {
                    ctx.cancel_requested = true;
                    return StateId::Idle;
                }
            }
        }
    }

    // Проверка максимального времени записи (15 с)
    uint32_t total_recorded_ms = static_cast<uint32_t>(
        (ctx.sample_bytes / BYTES_PER_SAMPLE) * 1000 / SAMPLE_RATE);
    if (total_recorded_ms >= m_max_record_ms) {
        return StateId::Sending;
    }

    return StateId::Stay;
}

void RecordingState::on_interrupt(VoiceContext& ctx, const char* reason) {
    if (!reason) return;
    if (std::strcmp(reason, "button_stop") == 0 || std::strcmp(reason, "stop_and_send") == 0) {
        ctx.stop_requested = true;
    } else if (std::strcmp(reason, "cancel") == 0) {
        ctx.cancel_requested = true;
    }
}

void RecordingState::on_exit(VoiceContext& ctx) {
    ctx.stop_requested = false;
    ctx.speech_detected = false;
    ctx.silence_chunks = 0;
    ctx.followup_silence_chunks = 0;

    if (ctx.sample_bytes > 0 && !ctx.cancel_requested) {
        ctx.write_wav_header();
        ctx.result.duration_ms = static_cast<uint32_t>(
            (ctx.sample_bytes / BYTES_PER_SAMPLE) * 1000 / SAMPLE_RATE);
    }
}

} // namespace kouki::voice
