#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include "voice_types.hpp"
#include "voice_context.hpp"
#include "voice_state_base.hpp"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#endif

namespace kouki::voice {

struct FsmDiagnostics {
    StateId current_id;
    StateId previous_id;
    const char* current_name;
    const char* previous_name;
    const char* last_reason;
    uint32_t time_in_state_ms;
    uint32_t transition_count;
    uint32_t interrupt_count;
    size_t buffer_used;
    size_t buffer_capacity;
    uint32_t vad_silence_ms;
};

class VoiceStateManager {
public:
    VoiceStateManager();
    ~VoiceStateManager();

    // Запрет копирования
    VoiceStateManager(const VoiceStateManager&) = delete;
    VoiceStateManager& operator=(const VoiceStateManager&) = delete;

    bool init(kouki_voice_event_cb_t cb, void *user_ctx);
    void register_state(VoiceStateBase* state);

    void transition_to(StateId target, const char* reason);
    void interrupt(const char* reason);
    void step(const int16_t *pcm, size_t samples);

    StateId current_id() const noexcept { return m_current_id; }
    StateId previous_id() const noexcept { return m_previous_id; }
    const char* current_name() const noexcept;
    uint32_t time_in_current_state_ms() const noexcept;

    bool is_transition_allowed(StateId from, StateId to) const noexcept;

    VoiceContext& context() noexcept { return m_context; }
    const VoiceContext& context() const noexcept { return m_context; }

    FsmDiagnostics get_diagnostics() const noexcept;
    void get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result) const noexcept;

    void publish_state(StateId st, const kouki_voice_result_t *res = nullptr) noexcept;

    // Вспомогательная функция для получения времени в миллисекундах
    static uint64_t get_time_ms() noexcept;

private:
    void transition_to_unlocked(StateId target, const char* reason);

    VoiceContext m_context;
    std::array<VoiceStateBase*, STATE_COUNT> m_states{};
    VoiceStateBase* m_current_state = nullptr;

    StateId m_current_id = StateId::Idle;
    StateId m_previous_id = StateId::Idle;
    const char* m_last_transition_reason = "boot";

    uint64_t m_state_entered_time_ms = 0;
    uint32_t m_transition_count = 0;
    uint32_t m_interrupt_count = 0;

#ifdef ESP_PLATFORM
    SemaphoreHandle_t m_lock = nullptr;
#endif
};

} // namespace kouki::voice
