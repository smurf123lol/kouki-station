#include "voice_state_manager.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "app_tone.h"
#include "sdkconfig.h"
static const char *TAG = "kouki.fsm";
#else
#define ESP_LOGI(tag, fmt, ...) std::printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) std::printf("[%s:WARN] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) std::printf("[%s:ERR] " fmt "\n", tag, ##__VA_ARGS__)
static const char *TAG = "fsm";
#endif

namespace kouki::voice {

VoiceStateManager::VoiceStateManager() {
    m_states.fill(nullptr);
#ifdef ESP_PLATFORM
    m_lock = xSemaphoreCreateRecursiveMutex();
#endif
    m_state_entered_time_ms = get_time_ms();
}

VoiceStateManager::~VoiceStateManager() {
#ifdef ESP_PLATFORM
    if (m_lock) {
        vSemaphoreDelete(m_lock);
        m_lock = nullptr;
    }
#endif
}

uint64_t VoiceStateManager::get_time_ms() noexcept {
#ifdef ESP_PLATFORM
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
#else
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

bool VoiceStateManager::init(kouki_voice_event_cb_t cb, void *user_ctx) {
    m_context.listener_cb = cb;
    m_context.listener_ctx = user_ctx;
    m_current_id.store(StateId::Idle, std::memory_order_release);
    m_previous_id = StateId::Idle;
    m_last_transition_reason = "boot";
    m_transition_count = 0;
    m_interrupt_count = 0;
    m_state_entered_time_ms = get_time_ms();

    size_t idx = static_cast<size_t>(StateId::Idle);
    if (idx < m_states.size() && m_states[idx]) {
        m_current_state = m_states[idx];
        m_current_state->on_enter(m_context, "boot");
    }
    return true;
}

void VoiceStateManager::register_state(VoiceStateBase* state) {
    if (!state) return;
    int8_t id = static_cast<int8_t>(state->id());
    if (id >= 0 && static_cast<size_t>(id) < m_states.size()) {
        m_states[static_cast<size_t>(id)] = state;
        if (state->id() == current_id()) {
            m_current_state = state;
        }
    }
}

const char* VoiceStateManager::current_name() const noexcept {
    return state_id_name(current_id());
}

uint32_t VoiceStateManager::time_in_current_state_ms() const noexcept {
    uint64_t now = get_time_ms();
    return (now >= m_state_entered_time_ms) ? static_cast<uint32_t>(now - m_state_entered_time_ms) : 0;
}

bool VoiceStateManager::is_transition_allowed(StateId from, StateId to) const noexcept {
    if (to == StateId::Stay || to == from) {
        return false;
    }

    switch (from) {
        case StateId::Idle:
            // Из IDLE переход только в RECORDING (по кнопке или wake-word)
            return (to == StateId::Recording);

        case StateId::Recording:
            // Из RECORDING переход в SENDING (VAD/кнопка), IDLE (отмена/тишина) или ERROR
            return (to == StateId::Sending || to == StateId::Idle || to == StateId::Error);

        case StateId::Sending:
            // Из SENDING переход в DONE (HTTP 200), ERROR (ошибка сети/500) или IDLE (отмена)
            return (to == StateId::Done || to == StateId::Error || to == StateId::Idle);

        case StateId::Done:
            // Из DONE переход в FOLLOWUP (вопрос/подтверждение), IDLE или RECORDING (прерывание кнопкой "Слушать")
            return (to == StateId::Followup || to == StateId::Idle || to == StateId::Recording);

        case StateId::Followup:
            // Из FOLLOWUP переход в RECORDING (речь/wake), IDLE (таймаут/отмена) или ERROR
            return (to == StateId::Recording || to == StateId::Idle || to == StateId::Error);

        case StateId::Error:
            // Из ERROR переход в IDLE
            return (to == StateId::Idle);

        default:
            return false;
    }
}

void VoiceStateManager::transition_to_unlocked(StateId target, const char* reason) {
    const StateId cur = m_current_id.load(std::memory_order_acquire);
    if (target == cur || target == StateId::Stay) {
        return;
    }

    if (!is_transition_allowed(cur, target)) {
        ESP_LOGW(TAG, "Отклонен невалидный переход [%s -> %s] (причина: %s)",
                 current_name(), state_id_name(target), reason ? reason : "none");
        return;
    }

    uint32_t dur_ms = time_in_current_state_ms();
    ESP_LOGI(TAG, "[FSM: %s -> %s] причина: '%s', длительность: %u мс",
             current_name(), state_id_name(target), reason ? reason : "none", (unsigned)dur_ms);

    if (m_current_state) {
        m_current_state->on_exit(m_context);
    }

    m_previous_id = cur;
    m_current_id.store(target, std::memory_order_release);
    m_last_transition_reason = reason ? reason : "none";
    m_transition_count++;
    m_state_entered_time_ms = get_time_ms();

    size_t idx = static_cast<size_t>(target);
    if (idx < m_states.size()) {
        m_current_state = m_states[idx];
    } else {
        m_current_state = nullptr;
    }

    if (m_current_state) {
        m_current_state->on_enter(m_context, reason);
    }

    publish_state(target, &m_context.result);
}

void VoiceStateManager::transition_to(StateId target, const char* reason) {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    transition_to_unlocked(target, reason);
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
}

void VoiceStateManager::interrupt(const char* reason) {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    ESP_LOGW(TAG, "[FSM: ПРЕРЫВАНИЕ в %s] причина: '%s'", current_name(), reason ? reason : "none");
    m_interrupt_count++;
    if (m_current_state) {
        m_current_state->on_interrupt(m_context, reason);
    }
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
}

bool VoiceStateManager::try_set_session(kouki_session_t session) noexcept {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    const StateId cur = m_current_id.load(std::memory_order_acquire);
    const bool ok = (cur != StateId::Recording && cur != StateId::Sending);
    if (ok) {
        m_context.session = session;
        m_context.dialogue_continue = false;
    }
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
    return ok;
}

kouki_session_t VoiceStateManager::get_session() const noexcept {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    const kouki_session_t session = m_context.session;
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
    return session;
}

bool VoiceStateManager::start_recording_from_button() noexcept {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    const StateId cur = m_current_id.load(std::memory_order_acquire);
    bool ok = true;
    if (cur == StateId::Sending) {
        ok = false;
    } else if (cur == StateId::Recording) {
        ok = true;
    } else if (cur == StateId::Idle) {
        transition_to_unlocked(StateId::Recording, "button_start");
    } else {
        ESP_LOGW(TAG, "[FSM: ПРЕРЫВАНИЕ в %s] причина: 'button_start'", current_name());
        m_interrupt_count++;
        if (m_current_state) {
            m_current_state->on_interrupt(m_context, "button_start");
        }
    }
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
    return ok;
}

bool VoiceStateManager::stop_and_send_from_button() noexcept {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    const StateId cur = m_current_id.load(std::memory_order_acquire);
    bool stopped = false;
    if (cur == StateId::Recording) {
        ESP_LOGW(TAG, "[FSM: ПРЕРЫВАНИЕ в %s] причина: 'button_stop'", current_name());
        m_interrupt_count++;
        if (m_current_state) {
            m_current_state->on_interrupt(m_context, "button_stop");
        }
        stopped = true;
    } else if (cur == StateId::Followup) {
        ESP_LOGW(TAG, "[FSM: ПРЕРЫВАНИЕ в %s] причина: 'cancel'", current_name());
        m_interrupt_count++;
        if (m_current_state) {
            m_current_state->on_interrupt(m_context, "cancel");
        }
    }
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
    return stopped;
}

void VoiceStateManager::step(const int16_t *pcm, size_t samples) {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    if (m_current_state) {
        StateId next = m_current_state->on_step(m_context, pcm, samples);
        const StateId cur = m_current_id.load(std::memory_order_acquire);
        if (next != StateId::Stay && next != cur) {
            transition_to_unlocked(next, "on_step");
        }
    }
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
}

FsmDiagnostics VoiceStateManager::get_diagnostics() const noexcept {
    FsmDiagnostics d{};
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    const StateId cur = m_current_id.load(std::memory_order_acquire);
    d.current_id = cur;
    d.previous_id = m_previous_id;
    d.current_name = state_id_name(cur);
    d.previous_name = state_id_name(m_previous_id);
    d.last_reason = m_last_transition_reason;
    d.time_in_state_ms = time_in_current_state_ms();
    d.transition_count = m_transition_count;
    d.interrupt_count = m_interrupt_count;
    d.buffer_used = m_context.sample_bytes;
    d.buffer_capacity = m_context.capacity_bytes;
    d.vad_silence_ms = m_context.last_vad_silence_ms;
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
    return d;
}

void VoiceStateManager::get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result) const noexcept {
#ifdef ESP_PLATFORM
    if (!m_lock || xSemaphoreTakeRecursive(m_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
#endif
    if (state) {
        *state = to_c_state(m_current_id.load(std::memory_order_acquire));
    }
    if (result) {
        *result = m_context.result;
    }
#ifdef ESP_PLATFORM
    xSemaphoreGiveRecursive(m_lock);
#endif
}

void VoiceStateManager::get_metrics(kouki_voice_metrics_t *metrics) const noexcept {
    if (!metrics) return;
#ifdef ESP_PLATFORM
    if (!m_lock || xSemaphoreTakeRecursive(m_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    constexpr uint32_t followup_limit_ms = CONFIG_KOUKI_FOLLOWUP_SILENCE_MS;
#else
    constexpr uint32_t followup_limit_ms = 1500;
#endif
    metrics->is_recording = (m_current_id.load(std::memory_order_acquire) == StateId::Recording);
    metrics->speech_detected = m_context.speech_detected;
    metrics->record_duration_ms = static_cast<uint32_t>(
        m_context.sample_bytes / (BYTES_PER_SAMPLE * (SAMPLE_RATE / 1000)));
    metrics->silence_ms = m_context.speech_detected ? m_context.last_vad_silence_ms : 0;
    metrics->silence_limit_ms = (m_context.dialogue_continue) ? followup_limit_ms : 3000;
#ifdef ESP_PLATFORM
    xSemaphoreGiveRecursive(m_lock);
#endif
}

void VoiceStateManager::publish_state(StateId st, const kouki_voice_result_t *res) noexcept {
    if (m_context.listener_cb) {
        m_context.listener_cb(to_c_state(st), res ? res : &m_context.result, m_context.listener_ctx);
    }
}

#if defined(CONFIG_KOUKI_TEST_CONSOLE_CMDS)
bool VoiceStateManager::force_done_for_test(const kouki_voice_result_t& res) {
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
#endif
    const StateId cur = m_current_id.load(std::memory_order_acquire);
    if (cur == StateId::Recording || cur == StateId::Sending) {
#ifdef ESP_PLATFORM
        if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
        return false;
    }
    if (m_current_state) {
        m_current_state->on_exit(m_context);
    }
    m_context.result = res;
    m_context.dialogue_continue = false;
    m_context.is_wake_recording = false;
    m_previous_id = cur;
    m_current_id.store(StateId::Done, std::memory_order_release);
    m_last_transition_reason = "test_mock_done";
    m_transition_count++;
    m_state_entered_time_ms = get_time_ms();
    size_t idx = static_cast<size_t>(StateId::Done);
    m_current_state = (idx < m_states.size()) ? m_states[idx] : nullptr;
    if (m_current_state) {
        m_current_state->on_enter(m_context, "test_mock_done");
    }
    publish_state(StateId::Done, &m_context.result);
#ifdef ESP_PLATFORM
    if (m_lock) xSemaphoreGiveRecursive(m_lock);
#endif
    return true;
}
#endif

} // namespace kouki::voice
