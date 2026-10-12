// SPDX-License-Identifier: GPL-2.0-or-later
// Launcher helper: use the same controller and prompt settings as the frontend,
// without opening a game window or initializing Vulkan.
#include "bbport_controller_prompts.h"
#include "bbport_controller_sdl.h"
#include "bbport_settings.h"
#include <array>
#include <cstdio>

int main() {
    BbSettings::Load();
    auto& s = BbSettings::Get();
    SDL_Gamepad* pad = nullptr;
    if (SDL_Init(SDL_INIT_GAMEPAD)) {
        int count = 0;
        auto* ids = SDL_GetGamepads(&count);
        if (ids && count) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    std::array<const char*, 4> labels{};
    if (pad) {
        for (int i = 0; i < 4; ++i) labels[i] = BbPrompts::SdlLabel(
            SDL_GetGamepadButtonLabel(pad, SDL_GamepadButton(i)));
    }
    const auto& map = pad && s.controller_device == BbPrompts::DeviceKey(pad)
                          ? s.controller_map : BbPrompts::Identity;
    // With no usable identity, retain original glyphs instead of guessing Xbox.
    const int layout = s.controller_layout == BbPrompts::Auto && !labels[0]
                           ? BbPrompts::PlayStation : s.controller_layout.load();
    std::printf("BB_CONTROLLER_LABELS=");
    for (int i = 0; i < 4; ++i) {
        std::printf("%s%s", i ? "," : "", BbPrompts::Label(layout, i, map, labels));
    }
    std::printf("\n");
    if (pad) SDL_CloseGamepad(pad);
    SDL_Quit();
}
