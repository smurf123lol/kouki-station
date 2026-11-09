#pragma once

#include "fsm/voice_state_base.hpp"
#include <atomic>

namespace kouki::voice {

constexpr uint64_t FOLLOWUP_SETTLE_GUARD_MS = 800;
constexpr uint32_t FOLLOWUP_SPEECH_STREAK_FRAMES = 3;
constexpr uint32_t FOLLOWUP_SPEECH_STREAK_CHUNKS = 2;

class FollowupState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "FOLLOWUP"; }
    StateId id() const noexcept override { return StateId::Followup; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

    // Проверяет устойчивое начало речи (минимум FOLLOWUP_SPEECH_STREAK_CHUNKS чанков подряд)
    bool evaluate_speech_chunk(const int16_t *pcm, size_t samples, uint32_t threshold) noexcept;
    uint32_t loud_streak() const noexcept { return m_loud_streak; }

private:
    uint64_t m_listen_after_ms = 0;
    uint64_t m_deadline_ms = 0;
    uint32_t m_loud_streak = 0;
    std::atomic<bool> m_cancel_requested{false};
    std::atomic<bool> m_start_requested{false};
};

} // namespace kouki::voice
