#pragma once

#include "fsm/voice_state_base.hpp"

namespace kouki::voice {

class RecordingState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "RECORDING"; }
    StateId id() const noexcept override { return StateId::Recording; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

private:
    uint64_t m_record_start_ms = 0;
    uint32_t m_min_amplitude = 120;
    uint32_t m_vad_silence_ms = 3000;
    uint32_t m_followup_silence_ms = 1500;
    uint32_t m_max_record_ms = 15000;
    bool m_followup_speech_confirmed = false;
};

} // namespace kouki::voice
