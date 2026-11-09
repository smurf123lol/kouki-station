#pragma once

#include "ui_state_base.hpp"

namespace kouki::ui {

class SubIdleState : public UiVoiceSubStateBase {
public:
    UiVoiceSubStateId id() const override { return UiVoiceSubStateId::SUB_IDLE; }
    void on_enter(UiContext& ctx) override;
    void on_exit(UiContext& ctx) override;
    void step(UiContext& ctx, uint32_t dt_ms) override;
    void on_tap(UiContext& ctx) override;

private:
    uint32_t m_time_poll_ms{1000};
    uint32_t m_last_wifi_seq{0};
};

class SubRecordingState : public UiVoiceSubStateBase {
public:
    UiVoiceSubStateId id() const override { return UiVoiceSubStateId::SUB_RECORDING; }
    void on_enter(UiContext& ctx) override;
    void on_exit(UiContext& ctx) override;
    void step(UiContext& ctx, uint32_t dt_ms) override;
    void on_tap(UiContext& ctx) override;
};

class SubSendingState : public UiVoiceSubStateBase {
public:
    UiVoiceSubStateId id() const override { return UiVoiceSubStateId::SUB_SENDING; }
    void on_enter(UiContext& ctx) override;
    void on_exit(UiContext& ctx) override;
    void step(UiContext& ctx, uint32_t dt_ms) override;
    void on_tap(UiContext& ctx) override;
};

class SubDoneState : public UiVoiceSubStateBase {
public:
    UiVoiceSubStateId id() const override { return UiVoiceSubStateId::SUB_DONE; }
    void on_enter(UiContext& ctx) override;
    void on_exit(UiContext& ctx) override;
    void step(UiContext& ctx, uint32_t dt_ms) override;
    void on_tap(UiContext& ctx) override;
};

class SubFollowupState : public UiVoiceSubStateBase {
public:
    UiVoiceSubStateId id() const override { return UiVoiceSubStateId::SUB_FOLLOWUP; }
    void on_enter(UiContext& ctx) override;
    void on_exit(UiContext& ctx) override;
    void step(UiContext& ctx, uint32_t dt_ms) override;
    void on_tap(UiContext& ctx) override;
};

class SubErrorState : public UiVoiceSubStateBase {
public:
    UiVoiceSubStateId id() const override { return UiVoiceSubStateId::SUB_ERROR; }
    void on_enter(UiContext& ctx) override;
    void on_exit(UiContext& ctx) override;
    void step(UiContext& ctx, uint32_t dt_ms) override;
    void on_tap(UiContext& ctx) override;

private:
    uint32_t m_timer_ms{2500};
};

} // namespace kouki::ui
