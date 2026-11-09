#pragma once

#include "fsm/voice_state_base.hpp"
#include <atomic>

namespace kouki::voice {

constexpr uint64_t FOLLOWUP_SETTLE_GUARD_MS = 800;
/* 390 мс непрерывной речи (13 кадров по 30 мс): бытовой шум и короткие
   скрипы такой цепочки не дают, а человек начинает фразу уверенно. */
constexpr uint32_t FOLLOWUP_SPEECH_STREAK_FRAMES = 13;
/* Предзапись 512 мс, чтобы не потерять первое слово продолжения. */
constexpr size_t FOLLOWUP_PREROLL_SAMPLES = 8192;

class FollowupState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "FOLLOWUP"; }
    StateId id() const noexcept override { return StateId::Followup; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

    // Проверяет устойчивое начало речи (минимум FOLLOWUP_SPEECH_STREAK_FRAMES кадров подряд)
    bool evaluate_speech_chunk(const int16_t *pcm, size_t samples, uint32_t threshold) noexcept;
    uint32_t loud_streak() const noexcept { return m_loud_streak; }

private:
    void push_preroll(const int16_t *pcm, size_t samples) noexcept;

    int16_t m_preroll[FOLLOWUP_PREROLL_SAMPLES] = {};
    size_t m_preroll_len = 0;
    uint64_t m_listen_after_ms = 0;
    uint64_t m_deadline_ms = 0;
    uint32_t m_loud_streak = 0;
    std::atomic<bool> m_cancel_requested{false};
    std::atomic<bool> m_start_requested{false};
};

} // namespace kouki::voice
