#include "done_state.hpp"
#include <cstdio>
#include <cstring>
#include <new>
#include "app_history.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
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

#if defined(ESP_PLATFORM) && defined(CONFIG_KOUKI_SPEECH_ENABLED)
struct SpeechWorkerArgs {
    DoneState *self;
    uint32_t gen;
    uint32_t speech_gen;
    char text[sizeof(kouki_voice_result_t::answer)];
};

static void trim_utf8_tail_speech(char *s) noexcept {
    if (!s || s[0] == '\0') return;
    const size_t len = std::strlen(s);
    const auto *u = reinterpret_cast<const unsigned char *>(s);
    size_t i = len;
    while (i > 0 && (u[i - 1] & 0xC0u) == 0x80u) {
        i--;
    }
    if (i == len) {
        if ((u[len - 1] & 0x80u) != 0u) {
            s[len - 1] = '\0';
        }
        return;
    }
    if (i == 0) {
        s[0] = '\0';
        return;
    }
    const unsigned char lead = u[i - 1];
    size_t expected = 0;
    if ((lead & 0xE0u) == 0xC0u) {
        expected = 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
        expected = 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
        expected = 4;
    } else {
        s[lead < 0x80u ? i : (i - 1)] = '\0';
        return;
    }
    if ((len - (i - 1)) < expected) {
        s[i - 1] = '\0';
    }
}

void DoneState::speech_worker_task(void *arg) {
    auto *args = static_cast<SpeechWorkerArgs *>(arg);
    DoneState *self = args ? args->self : nullptr;
    const uint32_t gen = args ? args->gen : 0;
    const uint32_t speech_gen = args ? args->speech_gen : 0;
    bool ok = false;

    if (args && args->text[0] != '\0' && self &&
        self->m_speech_gen.load() == gen && !self->m_interrupted.load()) {
        ok = (kouki_speech_play_gen(args->text, speech_gen) == ESP_OK);
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
        const uint32_t speech_gen = kouki_speech_reserve();
        auto *args = new (std::nothrow) SpeechWorkerArgs{};
        if (args) {
            args->self = this;
            args->gen = m_speech_gen.load();
            args->speech_gen = speech_gen;
            std::snprintf(args->text, sizeof(args->text), "%s", ctx.result.answer);
            trim_utf8_tail_speech(args->text);

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
    kouki_speech_abort();
#endif
    m_interrupted = true;
}

void DoneState::on_exit(VoiceContext& /*ctx*/) {
    m_speech_gen.fetch_add(1);
#ifdef ESP_PLATFORM
    kouki_speech_abort();
#endif
    m_speech_started = false;
    m_speech_finished = false;
    m_speech_ok = false;
    m_interrupted = false;
}

} // namespace kouki::voice
