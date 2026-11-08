#pragma once

#include <cstdint>

namespace kouki::ui {

enum class UiVoiceSubStateId {
    SUB_IDLE = 0,
    SUB_RECORDING,
    SUB_SENDING,
    SUB_DONE,
    SUB_FOLLOWUP,
    SUB_ERROR
};

enum class UiModalId {
    NONE = 0,
    SETUP,
    OTA
};

enum TileId {
    TILE_HISTORY = 0,
    TILE_VOICE = 1,
    TILE_SETTINGS = 2,
    TILE_COUNT = 3
};

} // namespace kouki::ui
