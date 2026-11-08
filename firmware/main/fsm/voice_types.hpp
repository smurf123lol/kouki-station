#pragma once

#include <cstdint>
#include <cstddef>
#include "app_voice.h"

namespace kouki::voice {

enum class StateId : int8_t {
    Stay = -1,
    Idle = KOUKI_VOICE_IDLE,
    Recording = KOUKI_VOICE_RECORDING,
    Sending = KOUKI_VOICE_SENDING,
    Done = KOUKI_VOICE_DONE,
    Error = KOUKI_VOICE_ERROR,
    Followup = KOUKI_VOICE_FOLLOWUP
};

constexpr size_t STATE_COUNT = 6;

inline const char* state_id_name(StateId id) noexcept {
    switch (id) {
        case StateId::Idle:      return "IDLE";
        case StateId::Recording: return "RECORDING";
        case StateId::Sending:   return "SENDING";
        case StateId::Done:      return "DONE";
        case StateId::Error:     return "ERROR";
        case StateId::Followup:  return "FOLLOWUP";
        case StateId::Stay:      return "STAY";
        default:                 return "UNKNOWN";
    }
}

inline kouki_voice_state_t to_c_state(StateId id) noexcept {
    switch (id) {
        case StateId::Idle:      return KOUKI_VOICE_IDLE;
        case StateId::Recording: return KOUKI_VOICE_RECORDING;
        case StateId::Sending:   return KOUKI_VOICE_SENDING;
        case StateId::Done:      return KOUKI_VOICE_DONE;
        case StateId::Error:     return KOUKI_VOICE_ERROR;
        case StateId::Followup:  return KOUKI_VOICE_FOLLOWUP;
        default:                 return KOUKI_VOICE_IDLE;
    }
}

inline StateId from_c_state(kouki_voice_state_t c_st) noexcept {
    switch (c_st) {
        case KOUKI_VOICE_IDLE:      return StateId::Idle;
        case KOUKI_VOICE_RECORDING: return StateId::Recording;
        case KOUKI_VOICE_SENDING:   return StateId::Sending;
        case KOUKI_VOICE_DONE:      return StateId::Done;
        case KOUKI_VOICE_ERROR:     return StateId::Error;
        case KOUKI_VOICE_FOLLOWUP:  return StateId::Followup;
        default:                    return StateId::Idle;
    }
}

} // namespace kouki::voice
