#include "done_state.hpp"
#include <cstring>
#include <new>
#include "app_history.h"

#ifdef ESP_PLATFORM
#include "app_speech.h"
#include "app_wifi.h"
#include "app_config.h"
#include "app_wake.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace kouki::voice {

#ifdef ESP_PLATFORM
struct SpeechWorkerArgs {
    DoneState *self;
    uint32_t gen;
    char text[512];
};

void DoneState::speech_worker_task(void *arg) {
    auto *args = static_cast<SpeechWorkerArgs *>(arg);
    DoneState *self = args ? args->self : nullptr;
    const uint32_t gen = args ? args->gen : 0;
    bool ok = false;

    if (args && args->text[0] != '\0' && self &&
        self->m_speech_gen.load() == gen && !self->m_interrupted.load()) {
        ok = (kouki_speech_play(args->text) == ESP_OK);
    }
    delete args;

    if (self && self->m_speech_gen.load() == gen) {
        self->m_speech_ok = ok;
        self->m_speech_finished = true;
    }
    vTaskDelete(nullptr);
}
#endif

void DoneState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_speech_gen.fetch_add(1);
    m_speech_started = false;
    m_speech_finished = false;
    m_speech_ok = false;
    m_interrupted = false;
    m_next_on_interrupt = StateId::Idle;

    if (ctx.result.transcript[0] != '\0' || ctx.result.answer[0] != '\0') {
        kouki_history_push(&ctx.result);
    }

#ifdef ESP_PLATFORM
    kouki_wake_reset();
    m_entered_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);

#if defined(CONFIG_KOUKI_SPEECH_ENABLED)
    if (ctx.result.answer[0] != '\0' && !kouki_config_is_silent()) {
        auto *args = new (std::nothrow) SpeechWorkerArgs{};
        if (args) {
            args->self = this;
            args->gen = m_speech_gen.load();
            std::strncpy(args->text, ctx.result.answer, sizeof(args->text) - 1);
            args->text[sizeof(args->text) - 1] = '\0';

            m_speech_started = true;
            m_speech_finished = false;
            m_speech_ok = false;

            if (xTaskCreate(speech_worker_task, "fsm.speech", 8192, args, 5, nullptr) != pdPASS) {
                delete args;
                m_speech_started = false;
                m_speech_finished = true;
            }
        } else {
            m_speech_finished = true;
        }
    } else {
        m_speech_finished = true;
    }
#else
    m_speech_finished = true;
#endif
#else
    (void)ctx;
    m_entered_ms = 0;
    m_speech_finished = true;
#endif
}

StateId DoneState::on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) {
#ifndef ESP_PLATFORM
    (void)pcm;
    (void)samples;
#endif
    if (m_interrupted.load()) {
        return m_next_on_interrupt.load();
    }

#ifdef ESP_PLATFORM
    // 1. Безусловно ждём завершения запущенного синтеза/воспроизведения речи (защита от гонки задач)
    if (m_speech_started.load() && !m_speech_finished.load()) {
        return StateId::Stay;
    }

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const bool has_answer = (ctx.result.answer[0] != '\0');
    const bool enable_followup = (has_answer && std::strcmp(ctx.result.intent, "ask") == 0) ||
                                 ctx.result.requires_confirmation
#if CONFIG_KOUKI_FOLLOWUP_TASK_ENABLED
                                 || true
#endif
                                 ;

    // 2. Если речь не озвучивалась (тихий режим, сбой TTS, [Задача записана]),
    //    удерживаем карточку минимум 2.5 с, чтобы её можно было прочесть.
    const bool tts_completed = m_speech_started.load() && m_speech_finished.load() && m_speech_ok.load();
    if (!tts_completed && (now - m_entered_ms) < DONE_CARD_HOLD_MS) {
        if (pcm && samples > 0 &&
            !kouki_speech_is_playing() && !kouki_speech_busy() &&
            kouki_wake_is_ready() && kouki_wake_feed(pcm, static_cast<int>(samples))) {
            ctx.is_wake_recording = true;
            ctx.dialogue_continue = false;
            return StateId::Recording;
        }
        return StateId::Stay;
    }

    if (enable_followup && kouki_wifi_is_connected() && CONFIG_KOUKI_FOLLOWUP_WINDOW_MS > 0) {
        return StateId::Followup;
    }
#else
    (void)ctx;
#endif

    ctx.dialogue_continue = false;
    return StateId::Idle;
}

void DoneState::on_interrupt(VoiceContext& /*ctx*/, const char* reason) {
    if (reason && (std::strcmp(reason, "button_start") == 0 || std::strcmp(reason, "start_recording") == 0)) {
        m_next_on_interrupt = StateId::Recording;
    } else {
        m_next_on_interrupt = StateId::Idle;
    }

#ifdef ESP_PLATFORM
    if (kouki_speech_is_playing() || kouki_speech_busy()) {
        kouki_speech_abort();
    }
#endif
    m_interrupted = true;
}

void DoneState::on_exit(VoiceContext& /*ctx*/) {
    m_speech_gen.fetch_add(1);
#ifdef ESP_PLATFORM
    if (kouki_speech_is_playing() || kouki_speech_busy()) {
        kouki_speech_abort();
    }
#endif
    m_speech_started = false;
    m_speech_finished = false;
    m_speech_ok = false;
    m_interrupted = false;
}

} // namespace kouki::voice
