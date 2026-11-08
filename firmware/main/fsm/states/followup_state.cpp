#include "followup_state.hpp"
#include <cstring>

#ifdef ESP_PLATFORM
#include "app_wake.h"
#include "app_config.h"
#include "app_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace kouki::voice {

void FollowupState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_cancel_requested = false;
    m_start_requested = false;

#ifdef ESP_PLATFORM
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    uint32_t window_ms = CONFIG_KOUKI_FOLLOWUP_WINDOW_MS > 0 ? CONFIG_KOUKI_FOLLOWUP_WINDOW_MS : 10000;
    m_deadline_ms = now + window_ms;
#else
    m_deadline_ms = 10000;
#endif
    ctx.dialogue_continue = false;
}

StateId FollowupState::on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) {
#ifndef ESP_PLATFORM
    (void)pcm;
    (void)samples;
#endif
    if (m_cancel_requested) {
        return StateId::Idle;
    }
    if (m_start_requested) {
        ctx.session = KOUKI_SESSION_ASK;
        ctx.is_wake_recording = false;
        ctx.dialogue_continue = false;
        return StateId::Recording;
    }

#ifdef ESP_PLATFORM
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (now >= m_deadline_ms) {
        return StateId::Idle; // Окно истекло
    }

    if (!kouki_wifi_is_connected()) {
        return StateId::Error; // Потеря сети в окне продолжения
    }

    if (pcm && samples > 0) {
        // 1) Проверяем кодовое слово «эй гин»
        if (kouki_wake_is_ready() && kouki_wake_feed(pcm, static_cast<int>(samples))) {
            ctx.is_wake_recording = true;
            ctx.dialogue_continue = false;
            return StateId::Recording;
        }

        // 2) Проверяем начало речи
        const uint32_t speech_chunk_th = (CONFIG_KOUKI_MIN_AMPLITUDE * 3) / 2;
        if (VoiceContext::is_chunk_loud(pcm, samples, speech_chunk_th)) {
            ctx.session = KOUKI_SESSION_ASK;
            ctx.dialogue_continue = true;
            ctx.is_wake_recording = false;
            return StateId::Recording;
        }
    }
#endif

    return StateId::Stay;
}

void FollowupState::on_interrupt(VoiceContext& /*ctx*/, const char* reason) {
    if (!reason) return;
    if (std::strcmp(reason, "button_start") == 0 || std::strcmp(reason, "start_recording") == 0) {
        m_start_requested = true;
    } else {
        m_cancel_requested = true;
    }
}

void FollowupState::on_exit(VoiceContext& /*ctx*/) {
    m_cancel_requested = false;
    m_start_requested = false;
}

} // namespace kouki::voice
