#include "recording_state.hpp"
#include "app_vad.h"
#include <cstring>
#include <cstdlib>

#ifdef ESP_PLATFORM
#include "app_tone.h"
#include "app_config.h"
#include "app_power.h"
#include "app_wake.h"
#include "app_wake_verify.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <esp_err.h>
static const char *TAG = "kouki.fsm";
#endif

namespace kouki::voice {

namespace {
#ifdef ESP_PLATFORM
constexpr uint32_t FOLLOWUP_MIN_VOICED_MS = CONFIG_KOUKI_FOLLOWUP_MIN_VOICED_MS;
#else
constexpr uint32_t FOLLOWUP_MIN_VOICED_MS = 500; /* значение Kconfig по умолчанию для host-тестов */
#endif
constexpr uint32_t TONE_GUARD_MS = 320;        /* хвост стартового тона */
constexpr uint32_t WAKE_VERIFY_AFTER_MS = 700; /* когда отдаём начало записи на ASR */
constexpr uint32_t WAKE_VERIFY_WAIT_MS = 7000; /* Whisper на CPU отвечает ~3 с, ждём с запасом */
/* Ниже этого пика кусок считаем тишиной/шумом и хит отклоняем без похода в ASR. */
constexpr int WAKE_VERIFY_MIN_PEAK = 700;
constexpr size_t TRIM_BEFORE_MS = 300;
constexpr size_t TRIM_AFTER_MS = 400;
} // namespace

void RecordingState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    ctx.reset_recording();
    ctx.result = {};
    m_followup_speech_confirmed = false;
    m_voiced_ms = 0;
    m_first_voiced_byte = 0;
    m_last_voiced_byte = 0;
    m_preroll_bytes = 0;
    m_speech_start_bytes = 0;
    m_verify_started = false;
    m_verify_started_at_ms = 0;
    m_verify_done = false;
    kouki_vad_reset();

#ifdef ESP_PLATFORM
    kouki_wake_verify_reset();
#endif

    /* Предзапись: последние ~1 с до хита. Без неё «эй гин» и первый слог
       команды остаются за кадром (детектор срабатывает на конце слова). */
#ifdef ESP_PLATFORM
    if (ctx.buffer && ctx.capacity_bytes > WAV_HEADER_SIZE) {
        const int max_samples = static_cast<int>((ctx.capacity_bytes - WAV_HEADER_SIZE) / BYTES_PER_SAMPLE);
        int n = 0;
        if (ctx.is_wake_recording) {
            n = kouki_wake_get_preroll(reinterpret_cast<int16_t*>(ctx.buffer + WAV_HEADER_SIZE), max_samples);
        } else if (ctx.pending_preroll && ctx.pending_preroll_samples > 0) {
            const size_t take = (ctx.pending_preroll_samples < static_cast<size_t>(max_samples))
                                    ? ctx.pending_preroll_samples
                                    : static_cast<size_t>(max_samples);
            std::memcpy(ctx.buffer + WAV_HEADER_SIZE, ctx.pending_preroll, take * BYTES_PER_SAMPLE);
            n = static_cast<int>(take);
        }
        if (n > 0) {
            ctx.sample_bytes = static_cast<size_t>(n) * BYTES_PER_SAMPLE;
            m_preroll_bytes = ctx.sample_bytes;
        }
    }
#endif
    ctx.pending_preroll = nullptr;
    ctx.pending_preroll_samples = 0;

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
    m_speech_start_bytes = m_preroll_bytes + static_cast<size_t>(TONE_GUARD_MS) * SAMPLE_RATE / 1000 * BYTES_PER_SAMPLE;

    /* Бип играем сразу: индикация «станция слушает» не должна ждать сети.
       На ASR уходит только кусок до бипа (pre-roll), поэтому бип ему не мешает. */
    m_wake_tone_pending = false;
    if (ctx.is_wake_recording) {
        kouki_tone_play(KOUKI_TONE_WAKE);
    } else {
        kouki_tone_play(KOUKI_TONE_START);
    }
#else
    m_followup_silence_ms = 1500;
    m_record_start_ms = 0;
    m_speech_start_bytes = static_cast<size_t>(TONE_GUARD_MS) * SAMPLE_RATE / 1000 * BYTES_PER_SAMPLE;
#endif
}

StateId RecordingState::on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) {
    if (ctx.cancel_requested) {
        return StateId::Idle;
    }
    if (ctx.stop_requested) {
        return StateId::Sending;
    }

#ifdef ESP_PLATFORM
    /* ASR-подтверждение кодового слова: пока ответа нет, запись продолжается,
       а при отказе — тихо отменяем (без тона ошибки и без отправки на сервер). */
    if (m_verify_started) {
        const kouki_wake_verify_state_t st = kouki_wake_verify_state();
        if (st == KOUKI_WAKE_VERIFY_REJECTED) {
            ESP_LOGI(TAG, "ложный хит отменён ASR-проверкой (услышано: «%s»)", kouki_wake_verify_last_transcript());
            kouki_wake_preroll_clear();
            ctx.cancel_requested = true;
            return StateId::Idle;
        }
        const uint64_t waited =
            static_cast<uint64_t>(esp_timer_get_time() / 1000) - m_verify_started_at_ms;
        if (st == KOUKI_WAKE_VERIFY_ACCEPTED || st == KOUKI_WAKE_VERIFY_UNAVAILABLE) {
            if (st == KOUKI_WAKE_VERIFY_ACCEPTED) {
                ESP_LOGI(TAG, "хит подтверждён распознаванием («%s»)", kouki_wake_verify_last_transcript());
            } else {
                ESP_LOGW(TAG, "ASR-проверка недоступна — принимаю локальный хит");
            }
            if (m_wake_tone_pending) {
                kouki_tone_play(KOUKI_TONE_WAKE);
                m_wake_tone_pending = false;
            }
            m_verify_started = false;
            m_verify_done = true;
        } else if (waited >= WAKE_VERIFY_WAIT_MS) {
            ESP_LOGW(TAG, "ответа ASR нет за %u мс — принимаю локальный хит", (unsigned)WAKE_VERIFY_WAIT_MS);
            if (m_wake_tone_pending) {
                kouki_tone_play(KOUKI_TONE_WAKE);
                m_wake_tone_pending = false;
            }
            m_verify_started = false;
            m_verify_done = true;
        }
    }
#endif

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
    if (is_vad_speech) {
        m_voiced_ms += static_cast<uint32_t>(static_cast<uint64_t>(samples) * 1000 / SAMPLE_RATE);
        if (m_first_voiced_byte == 0) {
            m_first_voiced_byte = (ctx.sample_bytes > bytes) ? (ctx.sample_bytes - bytes) : 0;
        }
        m_last_voiced_byte = ctx.sample_bytes;
    }

#ifdef ESP_PLATFORM
    /* Запускаем проверку, когда после pre-roll накопилось ~700 мс записи:
       этого хватает, чтобы в кусок попали «эй гин» и начало команды. */
    if (CONFIG_KOUKI_WAKE_VERIFY && ctx.is_wake_recording && !m_verify_started && !m_verify_done && ctx.buffer &&
        kouki_wake_verify_state() != KOUKI_WAKE_VERIFY_PENDING) {
        const size_t after_preroll = (ctx.sample_bytes > m_preroll_bytes) ? (ctx.sample_bytes - m_preroll_bytes) : 0;
        const size_t need = static_cast<size_t>(WAKE_VERIFY_AFTER_MS) * SAMPLE_RATE / 1000 * BYTES_PER_SAMPLE;
        if (after_preroll >= need) {
            int total_samples = static_cast<int>(ctx.sample_bytes / BYTES_PER_SAMPLE);
            /* Проверяем только pre-roll (слово до бипа): бип и команда в кусок
               не попадают, поэтому Whisper не слышит «1» вместо «эй гин». */
            const int word_samples = static_cast<int>(m_preroll_bytes / BYTES_PER_SAMPLE);
            if (word_samples > 0 && word_samples < total_samples) {
                total_samples = word_samples;
            }
            /* Уровень для диагностики: тихий кусок Whisper тоже «не слышит». */
            const int16_t *audio = reinterpret_cast<const int16_t*>(ctx.buffer + WAV_HEADER_SIZE);
            int peak = 0;
            int64_t sum = 0;
            int cnt = 0;
            for (int i = 0; i < total_samples; i += 8) {
                const int v = std::abs(static_cast<int>(audio[i]));
                if (v > peak) {
                    peak = v;
                }
                sum += v;
                ++cnt;
            }
            ESP_LOGI(TAG, "кусок на ASR: %d сэмплов, пик %d, средн %d",
                     total_samples, peak, cnt > 0 ? static_cast<int>(sum / cnt) : 0);

            /* Whisper плохо слышит фразу в окружении тишины: обрезаем кусок по
               границам громкого участка (запас 120 мс до и 200 мс после). */
            int first_loud = -1;
            int last_loud = -1;
            for (int i = 0; i < total_samples; ++i) {
                if (std::abs(static_cast<int>(audio[i])) > 300) {
                    if (first_loud < 0) {
                        first_loud = i;
                    }
                    last_loud = i;
                }
            }
            int from = 0;
            int to = total_samples;
            if (first_loud >= 0 && last_loud > first_loud) {
                from = first_loud - SAMPLE_RATE * 120 / 1000;
                if (from < 0) {
                    from = 0;
                }
                to = last_loud + SAMPLE_RATE * 200 / 1000;
                if (to > total_samples) {
                    to = total_samples;
                }
                ESP_LOGI(TAG, "кусок на ASR обрезан по речи: %d..%d сэмплов", from, to);
            }
            const int send_samples = to - from;

            /* Слишком тихий кусок: Whisper всё равно вернёт «(звук …)» — отклоняем локально,
               не тратя запрос и не отправляя мусор на сервер. */
            if (peak < WAKE_VERIFY_MIN_PEAK) {
                ESP_LOGI(TAG, "хит отклонён локально: кусок слишком тихий (пик %d < %d)", peak, WAKE_VERIFY_MIN_PEAK);
                kouki_wake_preroll_clear();
                ctx.cancel_requested = true;
                return StateId::Idle;
            }

            if (kouki_wake_verify_start(audio + from, send_samples) == ESP_OK) {
                m_verify_started = true;
                m_verify_started_at_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
                ESP_LOGI(TAG, "кусок отправлен на ASR-подтверждение кодового слова (%u мс, %d сэмплов)",
                         static_cast<unsigned>(ctx.sample_bytes * 1000 / BYTES_PER_SAMPLE / SAMPLE_RATE),
                         send_samples);
            } else {
                /* Wi-Fi ещё не поднялся или ASR занят: один раз пишем в лог
                   и принимаем локальный хит. */
                if (m_wake_tone_pending) {
                    kouki_tone_play(KOUKI_TONE_WAKE);
                    m_wake_tone_pending = false;
                }
                m_verify_started = true;
                m_verify_started_at_ms =
                    static_cast<uint64_t>(esp_timer_get_time() / 1000) - WAKE_VERIFY_WAIT_MS;
                ESP_LOGW(TAG, "ASR-проверка недоступна — принимаю локальный хит");
            }
        }
    }
#endif

    bool send_now = false;

    if (ctx.dialogue_continue) {
        if (is_vad_speech) {
            ctx.speech_detected = true;
            m_followup_speech_confirmed = true;
            ctx.followup_silence_chunks = 0;
            ctx.last_vad_silence_ms = 0;
        } else {
            ctx.followup_silence_chunks++;
            const uint32_t silence_ms = static_cast<uint32_t>(
                ctx.followup_silence_chunks * (static_cast<uint64_t>(samples) * 1000 / SAMPLE_RATE));
            ctx.last_vad_silence_ms = silence_ms;

            uint32_t total_recorded_ms = static_cast<uint32_t>(
                (ctx.sample_bytes / BYTES_PER_SAMPLE) * 1000 / SAMPLE_RATE);

            // Если за первые 2500 мс в follow-up речь ни разу не подтвердилась VAD (только шум) —
            // отменяем запись локально без отправки на сервер и без тона ошибки
            if (!m_followup_speech_confirmed && total_recorded_ms >= 2500) {
#ifdef ESP_PLATFORM
                ESP_LOGI(TAG, "follow-up отменён: VAD за 2500 мс не обнаружил речь человека");
#endif
                ctx.cancel_requested = true;
                return StateId::Idle;
            }

            if (silence_ms >= m_followup_silence_ms) {
                /* Мало подтверждённой речи — это шум/телевизор, на сервер не отправляем. */
                if (m_followup_speech_confirmed && m_voiced_ms >= FOLLOWUP_MIN_VOICED_MS) {
                    send_now = true;
                } else {
#ifdef ESP_PLATFORM
                    ESP_LOGI(TAG, "follow-up отменён: подтверждённой речи %u мс < %d мс",
                             static_cast<unsigned>(m_voiced_ms), CONFIG_KOUKI_FOLLOWUP_MIN_VOICED_MS);
#endif
                    ctx.cancel_requested = true;
                    return StateId::Idle;
                }
            }
        }
    } else {
        // Общая логика VAD для записи по кнопке и по wake-word:
        // первые TONE_GUARD_MS после тона не считаем речью (хвост сигнала).
        if (ctx.sample_bytes >= m_speech_start_bytes && is_vad_speech) {
            if (!ctx.speech_detected) {
                ctx.speech_detected = true;
            }
            ctx.silence_chunks = 0;
            ctx.last_vad_silence_ms = 0;
        } else {
            ctx.silence_chunks++;
            const uint32_t silence_ms = static_cast<uint32_t>(
                ctx.silence_chunks * (static_cast<uint64_t>(samples) * 1000 / SAMPLE_RATE));
            ctx.last_vad_silence_ms = silence_ms;

            if (ctx.speech_detected) {
                // Если речь была зафиксирована — через m_vad_silence_ms (3000 мс) завершаем запись
                if (silence_ms >= m_vad_silence_ms) {
                    send_now = true;
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
    const uint32_t total_recorded_ms = static_cast<uint32_t>(
        (ctx.sample_bytes / BYTES_PER_SAMPLE) * 1000 / SAMPLE_RATE);
    if (!send_now && total_recorded_ms >= m_max_record_ms) {
        send_now = true;
    }

#ifdef ESP_PLATFORM
    if (send_now && m_verify_started) {
        return StateId::Stay; // даём ASR-проверке договорить
    }
#endif
    return send_now ? StateId::Sending : StateId::Stay;
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
        /* Обрезка по границам речи: убираем длинные паузы/тишину в начале и
           в конце, чтобы на сервер уходила именно фраза. */
        if (ctx.buffer && m_first_voiced_byte > 0 && m_last_voiced_byte > m_first_voiced_byte) {
            const size_t before = TRIM_BEFORE_MS * SAMPLE_RATE / 1000 * BYTES_PER_SAMPLE;
            const size_t after = TRIM_AFTER_MS * SAMPLE_RATE / 1000 * BYTES_PER_SAMPLE;
            size_t start = (m_first_voiced_byte > before) ? (m_first_voiced_byte - before) : 0;
            size_t end = m_last_voiced_byte + after;
            if (end > ctx.sample_bytes) {
                end = ctx.sample_bytes;
            }
            if (start < end && (end - start) >= bytes_min()) {
                std::memmove(ctx.buffer + WAV_HEADER_SIZE, ctx.buffer + WAV_HEADER_SIZE + start, end - start);
                ctx.sample_bytes = end - start;
            }
        }
        ctx.write_wav_header();
        ctx.result.duration_ms = static_cast<uint32_t>(
            (ctx.sample_bytes / BYTES_PER_SAMPLE) * 1000 / SAMPLE_RATE);
    }

#ifdef ESP_PLATFORM
    /* После записи кольцо предзаписи ещё секунду держит произнесённое «эй гин»;
       без сброса и короткой паузы детектор может сработать вторично на хвосте
       той же фразы (в логе это выглядело как два перехода в RECORDING). */
    kouki_wake_preroll_clear();
    kouki_wake_mute(600);
#endif
}

size_t RecordingState::bytes_min() const noexcept {
    return static_cast<size_t>(500) * SAMPLE_RATE / 1000 * BYTES_PER_SAMPLE;
}

} // namespace kouki::voice
