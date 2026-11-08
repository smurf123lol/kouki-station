#pragma once

#include "fsm/voice_state_base.hpp"
#include <atomic>

namespace kouki::voice {

class ErrorState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "ERROR"; }
    StateId id() const noexcept override { return StateId::Error; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

private:
    uint64_t m_deadline_ms = 0;
    std::atomic<bool> m_dismissed{false};
};

} // namespace kouki::voice
