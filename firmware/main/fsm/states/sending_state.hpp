#pragma once

#include "fsm/voice_state_base.hpp"
#include <atomic>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace kouki::voice {

class SendingState final : public VoiceStateBase {
public:
    SendingState() = default;
    ~SendingState() override = default;

    const char* name() const noexcept override { return "SENDING"; }
    StateId id() const noexcept override { return StateId::Sending; }

    void on_enter(VoiceContext& ctx, const char* reason) override;
    StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) override;
    void on_interrupt(VoiceContext& ctx, const char* reason) override;
    void on_exit(VoiceContext& ctx) override;

    // Вызывается из воркера отправки или host-теста по завершении
    void notify_complete(StateId result_state) noexcept;

private:
    std::atomic<bool> m_in_progress{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<StateId> m_next_state{StateId::Stay};
    std::atomic<uint32_t> m_gen{0};
    std::atomic<bool> m_has_worker_result{false};
    kouki_voice_result_t m_worker_result{};

#ifdef ESP_PLATFORM
    TaskHandle_t m_task_handle = nullptr;
    static void send_worker_task(void *arg);
#endif
};

} // namespace kouki::voice
