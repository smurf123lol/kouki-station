#pragma once

#include "fsm/voice_state_base.hpp"

namespace kouki::voice {

class IdleState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "IDLE"; }
    StateId id() const noexcept override { return StateId::Idle; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

private:
    bool m_start_requested = false;
};

} // namespace kouki::voice
