#include "error_state.hpp"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if defined(CONFIG_KOUKI_SOUND_SIGNALS)
#include "app_tone.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace kouki::voice {

#if defined(ESP_PLATFORM) && defined(CONFIG_KOUKI_SOUND_SIGNALS)
static std::atomic<bool> s_err_tone_active{false};

static void error_tone_worker_task(void *arg) {
    (void)arg;
    kouki_tone_play(KOUKI_TONE_ERROR);
    s_err_tone_active.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}
#endif

void ErrorState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_dismissed = false;
    ctx.dialogue_continue = false;
#ifdef ESP_PLATFORM
#if defined(CONFIG_KOUKI_SOUND_SIGNALS)
    bool expected = false;
    if (s_err_tone_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        if (xTaskCreate(error_tone_worker_task, "fsm.err_tone", 4096, nullptr, 4, nullptr) != pdPASS) {
            s_err_tone_active.store(false, std::memory_order_release);
        }
    }
#endif
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    m_deadline_ms = now + 2000;
#else
    m_deadline_ms = 2000;
#endif
}

StateId ErrorState::on_step(VoiceContext& /*ctx*/, const int16_t* /*pcm*/, size_t /*samples*/) {
    if (m_dismissed) {
        return StateId::Idle;
    }

#ifdef ESP_PLATFORM
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    if (now >= m_deadline_ms) {
        return StateId::Idle;
    }
#endif

    return StateId::Stay;
}

void ErrorState::on_interrupt(VoiceContext& /*ctx*/, const char* /*reason*/) {
    m_dismissed = true;
}

void ErrorState::on_exit(VoiceContext& /*ctx*/) {
    m_dismissed = false;
}

} // namespace kouki::voice
