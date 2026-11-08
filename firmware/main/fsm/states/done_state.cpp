#include "done_state.hpp"
#include <cstring>

#ifdef ESP_PLATFORM
#include "app_speech.h"
#include "app_wifi.h"
#include "app_config.h"
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace kouki::voice {

#ifdef ESP_PLATFORM
void DoneState::speech_worker_task(void *arg) {
    auto *self = static_cast<DoneState *>(arg);
    if (self && self->m_answer_text[0] != '\0') {
        kouki_speech_play(self->m_answer_text);
    }
    if (self) {
        self->m_speech_finished = true;
        self->m_task_handle = nullptr;
    }
    vTaskDelete(nullptr);
}
#endif

void DoneState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_speech_started = false;
    m_speech_finished = false;
    m_interrupted = false;
    m_next_on_interrupt = StateId::Idle;
    m_answer_text[0] = '\0';

#ifdef ESP_PLATFORM
    m_entered_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);

    if (ctx.result.answer[0] != '\0') {
        std::strncpy(m_answer_text, ctx.result.answer, sizeof(m_answer_text) - 1);
        m_answer_text[sizeof(m_answer_text) - 1] = '\0';
        m_speech_started = true;
        m_speech_finished = false;

        if (xTaskCreate(speech_worker_task, "fsm.speech", 8192, this, 4, &m_task_handle) != pdPASS) {
            m_speech_finished = true;
            m_task_handle = nullptr;
        }
    } else {
        m_speech_finished = true;
    }
#else
    (void)ctx;
    m_entered_ms = 0;
    m_speech_finished = true;
#endif
}

StateId DoneState::on_step(VoiceContext& ctx, const int16_t* /*pcm*/, size_t /*samples*/) {
    if (m_interrupted.load()) {
        return m_next_on_interrupt.load();
    }

#ifdef ESP_PLATFORM
    // Если речь всё ещё воспроизводится — остаёмся в DONE, не блокируя аудиоцикл
    if (m_speech_started.load() && !m_speech_finished.load() && (kouki_speech_is_playing() || kouki_speech_busy())) {
        return StateId::Stay;
    }

    bool enable_followup = (std::strcmp(ctx.result.intent, "ask") == 0) ||
                           ctx.result.requires_confirmation
#if CONFIG_KOUKI_FOLLOWUP_TASK_ENABLED
                           || true
#endif
                           ;

    if (enable_followup && kouki_wifi_is_connected() && CONFIG_KOUKI_FOLLOWUP_WINDOW_MS > 0) {
        ctx.followup_until_ms = static_cast<uint32_t>(
            (esp_timer_get_time() / 1000) + CONFIG_KOUKI_FOLLOWUP_WINDOW_MS);
        return StateId::Followup;
    }
#else
    (void)ctx;
#endif

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
#ifdef ESP_PLATFORM
    if (kouki_speech_is_playing() || kouki_speech_busy()) {
        kouki_speech_abort();
    }
#endif
    m_speech_started = false;
    m_speech_finished = false;
    m_interrupted = false;
}

} // namespace kouki::voice
