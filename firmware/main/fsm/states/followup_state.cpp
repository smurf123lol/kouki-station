#include "followup_state.hpp"
#include <cstring>

#ifdef ESP_PLATFORM
#include "app_wake.h"
#include "app_config.h"
#include "app_speech.h"
#include "app_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace kouki::voice {

bool FollowupState::evaluate_speech_chunk(const int16_t *pcm, size_t samples, uint32_t threshold) noexcept {
    if (VoiceContext::is_chunk_loud(pcm, samples, threshold)) {
        m_loud_streak++;
        return m_loud_streak >= FOLLOWUP_SPEECH_STREAK_CHUNKS;
    }
    m_loud_streak = 0;
    return false;
}

void FollowupState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_cancel_requested = false;
    m_start_requested = false;
    m_loud_streak = 0;
    ctx.dialogue_continue = false;

#ifdef ESP_PLATFORM
    kouki_wake_reset();
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    const uint32_t window_ms = (CONFIG_KOUKI_FOLLOWUP_WINDOW_MS > 0) ? CONFIG_KOUKI_FOLLOWUP_WINDOW_MS : 10000;
    m_listen_after_ms = now + FOLLOWUP_SETTLE_GUARD_MS;
    m_deadline_ms = m_listen_after_ms + window_ms;
#else
    m_listen_after_ms = FOLLOWUP_SETTLE_GUARD_MS;
    m_deadline_ms = m_listen_after_ms + 10000;
#endif
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
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (now >= m_deadline_ms) {
        return StateId::Idle; // Окно истекло
    }

    if (!kouki_wifi_is_connected()) {
        return StateId::Error; // Потеря сети в окне продолжения
    }

    // Защита от самовозбуждения: пока динамик ещё звучит или реверберация не утихла — не слушаем
    if (kouki_speech_is_playing() || kouki_speech_busy() || now < m_listen_after_ms) {
        m_loud_streak = 0;
        return StateId::Stay;
    }

    if (pcm && samples > 0) {
        // 1) Проверяем кодовое слово «эй гин»
        if (kouki_wake_is_ready() && kouki_wake_feed(pcm, static_cast<int>(samples))) {
            ctx.is_wake_recording = true;
            ctx.dialogue_continue = false;
            return StateId::Recording;
        }

        // 2) Проверяем устойчивое начало речи (минимум 2 громких чанка подряд ~128 мс)
        const uint32_t speech_chunk_th = (CONFIG_KOUKI_MIN_AMPLITUDE * 3) / 2;
        if (evaluate_speech_chunk(pcm, samples, speech_chunk_th)) {
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
    m_loud_streak = 0;
}

} // namespace kouki::voice
