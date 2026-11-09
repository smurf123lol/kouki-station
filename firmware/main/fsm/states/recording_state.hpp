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

    /* Сколько мс VAD подтверждал речь, и где она началась/кончилась в буфере. */
    uint32_t m_voiced_ms = 0;
    size_t m_first_voiced_byte = 0;
    size_t m_last_voiced_byte = 0;

    /* Сколько байт в начале буфера заняла предзапись и до какого места
       действует защита от хвоста стартового тона. */
    size_t m_preroll_bytes = 0;
    size_t m_speech_start_bytes = 0;

    /* ASR-подтверждение кодового слова. */
    bool m_wake_tone_pending = false;
    bool m_verify_started = false;
    /* Проверка на текущую запись уже запускалась (повторно не запускаем:
       иначе на одну фразу уходит два запроса и второй может отменить запись). */
    bool m_verify_done = false;
    uint64_t m_verify_started_at_ms = 0;

    size_t bytes_min() const noexcept;
};

} // namespace kouki::voice
