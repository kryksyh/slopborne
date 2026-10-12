// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <SDL3/SDL.h>
#include <string>

namespace BbPrompts {
inline const char* SdlLabel(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
    default: return nullptr;
    }
}
inline std::string DeviceKey(SDL_Gamepad* pad) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(pad)), guid, sizeof(guid));
    return std::string(guid) + ":" + std::to_string(SDL_GetRealGamepadType(pad));
}
}
