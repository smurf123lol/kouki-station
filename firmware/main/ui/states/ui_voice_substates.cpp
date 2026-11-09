#include "ui_voice_substates.hpp"
#include "ui_state_manager.hpp"
#include "widgets/ui_orb_widget.hpp"
#include "widgets/ui_card_widget.hpp"
#include "app_time.h"
#include "app_wifi.h"
#include "app_speech.h"
#include "app_voice.h"
#include "esp_log.h"

static const char *TAG = "kouki.ui.states";

namespace kouki::ui {

// --------------------------------------------------------------------------
// SubIdleState
// --------------------------------------------------------------------------
void SubIdleState::on_enter(UiContext& ctx)
{
    ESP_LOGD(TAG, "Enter SubIdle");
    if (ctx.orb != nullptr) {
        ctx.orb->set_mode(UiVoiceSubStateId::SUB_IDLE);
        m_last_wifi_seq = kouki_wifi_state_seq();
        ctx.orb->update_wifi_status(kouki_wifi_is_connected());
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->hide();
    }
    if (ctx.manager != nullptr) {
        ctx.manager->unlock_scroll();
    }
    m_time_poll_ms = 1000; // немедленно обновить время
}

void SubIdleState::on_exit(UiContext& ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Exit SubIdle");
}

void SubIdleState::step(UiContext& ctx, uint32_t dt_ms)
{
    if (ctx.orb != nullptr) {
        ctx.orb->step(dt_ms);

        const uint32_t wifi_seq = kouki_wifi_state_seq();
        if (wifi_seq != m_last_wifi_seq) {
            m_last_wifi_seq = wifi_seq;
            ctx.orb->update_wifi_status(kouki_wifi_is_connected());
        }

        m_time_poll_ms += dt_ms;
        if (m_time_poll_ms >= 1000) {
            m_time_poll_ms = 0;
            char hm[16] = {0};
            char date[64] = {0};
            kouki_time_format_hm(hm, sizeof(hm));
            kouki_time_format_date(date, sizeof(date));
            ctx.orb->update_time(hm, date);
        }
    }
}

void SubIdleState::on_tap(UiContext& ctx)
{
    if (ctx.manager != nullptr) {
        ctx.manager->toggle_recording();
    }
}

// --------------------------------------------------------------------------
// SubRecordingState
// --------------------------------------------------------------------------
void SubRecordingState::on_enter(UiContext& ctx)
{
    ESP_LOGD(TAG, "Enter SubRecording");
    if (ctx.manager != nullptr) {
        ctx.manager->lock_scroll();
        ctx.manager->focus_voice_tile(false); // Мгновенный возврат на Tile 1 без анимации
    }
    if (ctx.orb != nullptr) {
        ctx.orb->set_mode(UiVoiceSubStateId::SUB_RECORDING);
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->hide();
    }
}

void SubRecordingState::on_exit(UiContext& ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Exit SubRecording");
}

void SubRecordingState::step(UiContext& ctx, uint32_t dt_ms)
{
    if (ctx.orb != nullptr) {
        ctx.orb->step(dt_ms);
        ctx.orb->update_recording_metrics(
            ctx.voice_metrics.record_duration_ms,
            ctx.voice_metrics.speech_detected,
            ctx.voice_metrics.silence_ms,
            ctx.voice_metrics.silence_limit_ms
        );
    }
}

void SubRecordingState::on_tap(UiContext& ctx)
{
    if (ctx.manager != nullptr) {
        ctx.manager->toggle_recording();
    }
}

// --------------------------------------------------------------------------
// SubSendingState
// --------------------------------------------------------------------------
void SubSendingState::on_enter(UiContext& ctx)
{
    ESP_LOGD(TAG, "Enter SubSending");
    if (ctx.manager != nullptr) {
        ctx.manager->lock_scroll();
    }
    if (ctx.orb != nullptr) {
        ctx.orb->set_mode(UiVoiceSubStateId::SUB_SENDING);
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->hide();
    }
}

void SubSendingState::on_exit(UiContext& ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Exit SubSending");
}

void SubSendingState::step(UiContext& ctx, uint32_t dt_ms)
{
    if (ctx.orb != nullptr) {
        ctx.orb->step(dt_ms);
    }
}

void SubSendingState::on_tap(UiContext& ctx)
{
    (void)ctx;
}

// --------------------------------------------------------------------------
// SubDoneState
// --------------------------------------------------------------------------
void SubDoneState::on_enter(UiContext& ctx)
{
    ESP_LOGD(TAG, "Enter SubDone");
    if (ctx.manager != nullptr) {
        ctx.manager->unlock_scroll();
    }
    if (ctx.orb != nullptr) {
        ctx.orb->set_mode(UiVoiceSubStateId::SUB_DONE);
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->set_result(ctx.voice_result);
        ctx.response_card->show();
    }
}

void SubDoneState::on_exit(UiContext& ctx)
{
    if (ctx.response_card != nullptr) {
        ctx.response_card->hide();
    }
    ESP_LOGD(TAG, "Exit SubDone");
}

void SubDoneState::step(UiContext& ctx, uint32_t dt_ms)
{
    if (ctx.orb != nullptr) {
        ctx.orb->step(dt_ms);
        ctx.orb->update_speech_pulse(kouki_speech_is_playing());
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->step(dt_ms);
    }
}

void SubDoneState::on_tap(UiContext& ctx)
{
    if (ctx.manager != nullptr) {
        ctx.manager->toggle_recording();
    }
}

// --------------------------------------------------------------------------
// SubFollowupState
// --------------------------------------------------------------------------
void SubFollowupState::on_enter(UiContext& ctx)
{
    ESP_LOGD(TAG, "Enter SubFollowup");
    if (ctx.manager != nullptr) {
        ctx.manager->unlock_scroll();
    }
    if (ctx.orb != nullptr) {
        ctx.orb->set_mode(UiVoiceSubStateId::SUB_FOLLOWUP);
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->hide();
    }
}

void SubFollowupState::on_exit(UiContext& ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Exit SubFollowup");
}

void SubFollowupState::step(UiContext& ctx, uint32_t dt_ms)
{
    if (ctx.orb != nullptr) {
        ctx.orb->step(dt_ms);
    }
}

void SubFollowupState::on_tap(UiContext& ctx)
{
    if (ctx.manager != nullptr) {
        ctx.manager->toggle_recording();
    }
}

// --------------------------------------------------------------------------
// SubErrorState
// --------------------------------------------------------------------------
void SubErrorState::on_enter(UiContext& ctx)
{
    ESP_LOGD(TAG, "Enter SubError");
    m_timer_ms = 2500;
    if (ctx.manager != nullptr) {
        ctx.manager->unlock_scroll();
    }
    if (ctx.orb != nullptr) {
        ctx.orb->set_mode(UiVoiceSubStateId::SUB_ERROR);
        ctx.orb->update_error_text(ctx.voice_result.error);
    }
    if (ctx.response_card != nullptr) {
        ctx.response_card->hide();
    }
}

void SubErrorState::on_exit(UiContext& ctx)
{
    (void)ctx;
    ESP_LOGD(TAG, "Exit SubError");
}

void SubErrorState::step(UiContext& ctx, uint32_t dt_ms)
{
    if (ctx.orb != nullptr) {
        ctx.orb->step(dt_ms);
    }
    if (m_timer_ms > dt_ms) {
        m_timer_ms -= dt_ms;
    } else {
        m_timer_ms = 0;
        if (ctx.manager != nullptr) {
            ctx.manager->transition_to(UiVoiceSubStateId::SUB_IDLE);
        }
    }
}

void SubErrorState::on_tap(UiContext& ctx)
{
    kouki_voice_dismiss_error();
    if (ctx.manager != nullptr) {
        ctx.manager->transition_to(UiVoiceSubStateId::SUB_IDLE);
    }
}

} // namespace kouki::ui
