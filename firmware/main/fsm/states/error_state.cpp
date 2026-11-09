#include "error_state.hpp"

#ifdef ESP_PLATFORM
#include "app_tone.h"
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace kouki::voice {

void ErrorState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_dismissed = false;
    ctx.dialogue_continue = false;
#ifdef ESP_PLATFORM
    kouki_tone_play(KOUKI_TONE_ERROR);
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    m_deadline_ms = now + 2000;
#else
    m_deadline_ms = 2000;
#endif
}

StateId ErrorState::on_step(VoiceContext& /*ctx*/, const int16_t* /*pcm*/, size_t /*samples*/) {
    if (m_dismissed) {
        return StateId::Idle;
    }

#ifdef ESP_PLATFORM
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (now >= m_deadline_ms) {
        return StateId::Idle;
    }
#endif

    return StateId::Stay;
}

void ErrorState::on_interrupt(VoiceContext& /*ctx*/, const char* /*reason*/) {
    m_dismissed = true;
}

void ErrorState::on_exit(VoiceContext& /*ctx*/) {
    m_dismissed = false;
}

} // namespace kouki::voice
