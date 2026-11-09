#pragma once

#include "fsm/voice_state_base.hpp"
#include <cstdint>

namespace kouki::voice {

constexpr uint64_t IDLE_SETTLE_GUARD_MS = 500;

class IdleState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "IDLE"; }
    StateId id() const noexcept override { return StateId::Idle; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

    uint64_t ignore_audio_until_ms() const noexcept { return m_ignore_audio_until_ms; }

private:
    bool m_start_requested = false;
    uint64_t m_ignore_audio_until_ms = 0;
};

} // namespace kouki::voice
