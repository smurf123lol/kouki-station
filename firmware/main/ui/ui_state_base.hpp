#pragma once

#include "ui_types.hpp"
#include "app_voice.h"

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace kouki::ui {

class UiStateManager;
class OrbWidget;
class CardWidget;

struct UiContext {
    UiStateManager* manager{nullptr};
    lv_obj_t* tileview{nullptr};
    lv_obj_t* tile_history{nullptr};
    lv_obj_t* tile_voice{nullptr};
    lv_obj_t* tile_settings{nullptr};
    OrbWidget* orb{nullptr};
    CardWidget* response_card{nullptr};

    kouki_voice_state_t voice_state{KOUKI_VOICE_IDLE};
    kouki_voice_result_t voice_result{};
    kouki_voice_metrics_t voice_metrics{};
};

class UiVoiceSubStateBase {
public:
    virtual ~UiVoiceSubStateBase() = default;

    virtual UiVoiceSubStateId id() const = 0;
    virtual void on_enter(UiContext& ctx) = 0;
    virtual void on_exit(UiContext& ctx) = 0;
    virtual void step(UiContext& ctx, uint32_t dt_ms) = 0;
    virtual void on_tap(UiContext& ctx) {}
};

} // namespace kouki::ui
