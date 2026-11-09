#include "idle_state.hpp"
#include <cstring>

#ifdef ESP_PLATFORM
#include "app_wake.h"
#include "app_config.h"
#include "app_speech.h"
#include "app_vad.h"
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace kouki::voice {

void IdleState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_start_requested = false;
    ctx.stop_requested = false;
    ctx.cancel_requested = false;
    ctx.is_wake_recording = false;
    ctx.dialogue_continue = false;
    ctx.followup_streak = 0;

#ifdef ESP_PLATFORM
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    m_ignore_audio_until_ms = now + IDLE_SETTLE_GUARD_MS;
    kouki_wake_reset();
    kouki_vad_reset();
    if (!kouki_wake_is_ready() && kouki_config_get_wake_enabled()) {
        kouki_wake_init();
    }
#else
    m_ignore_audio_until_ms = IDLE_SETTLE_GUARD_MS;
#endif
}

StateId IdleState::on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) {
#ifndef ESP_PLATFORM
    (void)pcm;
    (void)samples;
#endif
    if (m_start_requested) {
        m_start_requested = false;
        ctx.is_wake_recording = false;
        ctx.dialogue_continue = false;
        return StateId::Recording;
    }

#ifdef ESP_PLATFORM
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);

    // Во время активной речи динамика, инициализации или защитного окна (500 мс settle guard) не слушаем вейк-ворд
    if (kouki_speech_is_playing() || kouki_speech_busy() || now < m_ignore_audio_until_ms) {
        kouki_wake_reset();
        kouki_vad_reset();
        return StateId::Stay;
    }

    if (pcm && samples > 0 && kouki_wake_is_ready()) {
        if (kouki_wake_feed(pcm, static_cast<int>(samples))) {
            ctx.is_wake_recording = true;
            ctx.dialogue_continue = false;
            return StateId::Recording;
        }
    }
#endif

    return StateId::Stay;
}

void IdleState::on_interrupt(VoiceContext& /*ctx*/, const char* reason) {
    if (reason && (std::strcmp(reason, "button_start") == 0 || std::strcmp(reason, "start_recording") == 0)) {
        m_start_requested = true;
    }
}

void IdleState::on_exit(VoiceContext& /*ctx*/) {
    m_start_requested = false;
}

} // namespace kouki::voice
