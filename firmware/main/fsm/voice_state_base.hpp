#pragma once

#include "voice_types.hpp"
#include "voice_context.hpp"

namespace kouki::voice {

class VoiceStateManager;

class VoiceStateBase {
public:
    virtual ~VoiceStateBase() = default;

    virtual const char* name() const noexcept = 0;
    virtual StateId id() const noexcept = 0;

    virtual void on_enter(VoiceContext& /*ctx*/, const char* /*reason*/) {}
    virtual StateId on_step(VoiceContext& ctx, const int16_t *pcm, size_t samples) = 0;
    virtual void on_interrupt(VoiceContext& /*ctx*/, const char* /*reason*/) {}
    virtual void on_exit(VoiceContext& /*ctx*/) {}
};

} // namespace kouki::voice
