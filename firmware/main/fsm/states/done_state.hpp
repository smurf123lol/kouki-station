#pragma once

#include "fsm/voice_state_base.hpp"
#include <atomic>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace kouki::voice {

constexpr uint64_t DONE_CARD_HOLD_MS = 2500;

class DoneState final : public VoiceStateBase {
public:
    const char* name() const noexcept override { return "DONE"; }
    StateId id() const noexcept override { return StateId::Done; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

private:
#ifdef ESP_PLATFORM
    static void speech_worker_task(void *arg);
    TaskHandle_t m_task_handle{nullptr};
#endif
    std::atomic<uint32_t> m_speech_gen{0};
    std::atomic<bool> m_speech_started{false};
    std::atomic<bool> m_speech_finished{false};
    std::atomic<bool> m_speech_ok{false};
    std::atomic<bool> m_interrupted{false};
    std::atomic<StateId> m_next_on_interrupt{StateId::Idle};
    uint64_t m_entered_ms = 0;
};

} // namespace kouki::voice
