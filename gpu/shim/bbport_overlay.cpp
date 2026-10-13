// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "bbport_controller_sdl.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#elif defined(__APPLE__)
asm(".section __TEXT,__const\n"
    ".balign 16\n"
    ".private_extern _bb_font_ttf\n"
    "_bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".private_extern _bb_font_ttf_end\n"
    "_bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

SDL_Gamepad* prompt_pad = nullptr; // own SDL reference; window thread opens/closes it
SDL_JoystickID prompt_id = 0;
std::atomic<bool> prompt_connected{false};
std::string prompt_name, prompt_device;
std::array<const char*, 4> detected_labels{};
Uint64 next_controller_scan = 0;
bool calibrating = false;
BbPrompts::Calibration calibration;

const char* FaceLabel(int logical) {
    const auto& s = BbSettings::Get();
    const auto& map = !prompt_device.empty() && s.controller_device == prompt_device
                          ? s.controller_map : BbPrompts::Identity;
    return BbPrompts::Label(s.controller_layout, logical, map, detected_labels);
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value) calibrating = false;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Menu text is English, Russian when the launcher or the system language is Russian
// (BB_UI_LANGUAGE: "ru", "en"; unset: the system's preferred language).
const char* T(const char* text) {
    static const bool russian = [] {
        if (const char* env = std::getenv("BB_UI_LANGUAGE"); env && env[0]) {
            return std::strncmp(env, "ru", 2) == 0;
        }
        int count = 0;
        SDL_Locale** locales = SDL_GetPreferredLocales(&count);
        const bool ru = locales && count > 0 && locales[0]->language &&
                        std::strcmp(locales[0]->language, "ru") == 0;
        SDL_free(locales);
        return ru;
    }();
    if (!russian) {
        return text;
    }
    static const std::unordered_map<std::string_view, const char*> table = {
#include "bbport_overlay_ru.inc"
    };
    const auto it = table.find(text);
    return it != table.end() ? it->second : text;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void ControllerMenu() {
    if (!ImGui::CollapsingHeader(T("Controller buttons"))) return;
    auto& s = BbSettings::Get();
    if (prompt_connected) {
        ImGui::TextWrapped("%s", prompt_name.c_str());
    } else {
        ImGui::TextUnformatted(T("No controller connected"));
    }
    static const char* layouts[] = {"Automatic", "PlayStation", "Xbox", "Nintendo", "Positions"};
    const int layout = s.controller_layout;
    ImGui::BeginDisabled(calibrating);
    if (ImGui::BeginCombo(T("Button labels"), T(layouts[layout]))) {
        for (int i = 0; i < BbPrompts::LayoutCount; ++i) {
            if (ImGui::Selectable(T(layouts[i]), i == layout)) Store(s.controller_layout, i, true);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", T("Steam or controller firmware can hide the printed layout. "
                               "Choose Nintendo for Nintendo-style 8BitDo buttons."));
    ImGui::TextWrapped("%s", T("Button labels change the game glyphs after restart. Controls stay the same."));
    if (!prompt_connected) return;
    for (int i = 0; i < 4; ++i) {
        ImGui::Text("%s -> [%s]", T(BbPrompts::PsNames[i]), T(FaceLabel(i)));
    }
    ImGui::TextWrapped("%s", T("Game is still running while settings are open."));
    if (calibrating) {
        ImGui::TextUnformatted(T("Options/Menu or Esc cancels calibration."));
        ImGui::Text(T("Step %d of 4: press and release the physical %s face button."),
                    calibration.step + 1, T(BbPrompts::PositionNames[calibration.step]));
        if (calibration.held >= 0) ImGui::TextUnformatted(T("Release the button to continue."));
        if (calibration.error) {
            ImGui::TextWrapped("%s", T("That input is not a new face button. Try again; "
                                       "calibration supports four distinct face-button inputs."));
        }
        if (ImGui::Button(T("Cancel calibration"))) calibrating = false;
    } else {
        if (ImGui::Button(T("Calibrate remapped face buttons"))) {
            calibration = {};
            calibrating = true;
            ImGui::GetIO().ClearInputKeys();
        }
        if (s.controller_device == prompt_device) {
            ImGui::SameLine();
            if (ImGui::Button(T("Reset calibration"))) {
                s.controller_device.clear();
                s.controller_map = BbPrompts::Identity;
                dirty = true;
            }
            ImGui::TextUnformatted(T("Calibration saved for this controller type."));
        }
        ImGui::TextWrapped("%s", T("Recalibrate after changing Steam or firmware mappings."));
    }
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
#ifdef __APPLE__
    const char* title = T("Bloodborne settings  (Cmd+, / L3+R3)");
#else
    const char* title = T("Bloodborne settings  (Insert / L3+R3)");
#endif
    if (!ImGui::Begin(title, &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);

    ControllerMenu();

    ImGui::SeparatorText(T("Temporal upscaler"));
    static const char* upscalers[] = {T("Off"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
                                     T("TAA (native anti-aliasing)"), "DLSS (NVIDIA RTX)",
                                     "MetalFX (Apple)"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(T("Upscaler"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                : i == BbSettings::UpscalerMetalFx ? s.metalfx_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled(T("— not supported by this GPU"));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(T("— in progress"));
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), T("FSR 4 unavailable: %s"), problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(T("The mode selected above is active. FSR 4 can be selected "
                                     "again."));
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(T("FSR 4.1.1 in INT8 mode: the AMD 4.1.1 DLL model reproduced in Vulkan (output "
                   "matches the DLL). One model for Native..Performance and a separate one for "
                   "Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (needs the DLL and "
                   "Proton)."));
        } else {
            Hint(T("FSR 4 in INT8 mode (model v07 from the AMD FidelityFX SDK sources). Better "
                   "quality than FSR 3.1, but a heavier pass. Changing the preset rebuilds the "
                   "model (short pause). Assets: tools/fetch_fsr4_assets.sh."));
        }
        Checkbox(T("FSR 4: auto exposure"), s.fsr4_auto_exposure);
        Checkbox(T("FSR 4: inverted jitter sign"), s.fsr4_invert_jitter);
        Hint(T("For ghosting checks: the FSR 4 network normalizes color by exposure and uses it "
               "to decide when to discard past frames. Applied immediately, no restart."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(T("Preset"), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), T("%s (x%.1f, render %dx%d)"),
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped(T("TAA anti-aliases the scene at the output resolution, without the "
                             "FSR model or upscaling. The saved FSR preset comes back when FSR is "
                             "selected."));
    }
    ImGui::Text(T("Scene render size: %d x %d"), s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text(T("Preset at startup: %s"), BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(T("With output other than 1080p the whole game renders at the preset resolution "
                   "(patched at startup): fastest on Steam Deck and weaker GPUs. Preset and "
                   "output resolution changes apply after a restart. \"Live resolution change\" "
                   "below allows changes without a restart (post-processing then stays at 1080p, "
                   "slower)."));
        } else {
            Hint(T("BB_RENDER_RES fixes the scene size at startup. Remove this variable to change "
                   "resolution and presets without restarting the game."));
        }
    } else {
        Hint(T("Native AA: FSR works as anti-aliasing. The other presets lower the scene render "
               "resolution relative to the output. The interface is drawn at the output "
               "resolution. The preset applies from the next frame, no restart."));
    }
    Checkbox(T("Sharpening (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(T("Sharpness"), s.sharpness, 0.0f, 2.0f);
    Hint(T("Up to 1: the upscaler's own sharpening (RCAS). Above 1 an extra RCAS pass is added. "
           "Ctrl+click the slider to type an exact value."));
    ImGui::EndDisabled();
    Checkbox(T("Subpixel jitter"), s.jitter);
    Hint(T("Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
           "detail from several frames. Without it you only get history anti-aliasing."));

    ImGui::SeparatorText(T("Reactive mask"));
    ImGui::BeginDisabled(taa);
    Checkbox(T("Enable mask"), s.reactive);
    Hint(T("Marks transparent effects (particles, haze) so the upscaler relies less on past "
           "frames. Fewer trails behind effects, but shimmering returns under them."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(T("Scale"), s.reactive_scale, 0.0f, 4.0f);
    Slider(T("Threshold"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(T("Maximum"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(T("Show mask (debug)"), &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(T("Character motion vectors"), s.object_motion);
    Hint(T("Exact vectors for animated objects: clothing and weapons break up less in motion. A "
           "static scene gets no extra pass. Applies after a game restart."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(T("Show motion vectors (debug)"), &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(T("Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the pixel "
           "got an exact object vector, not just camera motion. A moving object with no blue and "
           "no red/green is treated as static by the upscaler, hence the trail."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(T("Output resolution"));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(T("Output resolution"), outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(T("Size of the final frame and interface. The preset sets the scene size relative to "
               "the output: 4K Performance = 1920x1080. Applies after a game restart."));
    } else {
        Hint(T("Size of the final frame and interface, changed at the next frame. The preset sets "
               "the scene size relative to the output: 4K Performance = 1920x1080. A size change "
               "resets the FSR history and may cause a short pause."));
    }
    static const char* render_sizes[] = {T("1920 x 1080 (native)"), "1600 x 900", "1280 x 720"};
    const int render_res = s.render_res;
    ImGui::BeginDisabled(s.output_res != BbSettings::OutputDefault);
    if (ImGui::BeginCombo(T("Render resolution"), render_sizes[render_res])) {
        for (int i = 0; i < BbSettings::RenderResCount; ++i) {
            if (ImGui::Selectable(render_sizes[i], i == render_res)) {
                BbSettings::SelectRenderRes(i);
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Hint(T("Scene size at 1080p output, patched at game start: the smaller sizes are much faster "
           "and the upscaler (MetalFX on Apple GPUs) fills 1080p. Applies after a game restart."));
    static const char* live_modes[] = {T("Auto (by GPU)"), T("Off (faster)"), T("On")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(T("Live resolution change"), live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint(T("On: output resolution and preset change without a restart, but the game's "
           "post-processing stays at 1080p, noticeably slower on Steam Deck and older GPUs. Off: "
           "everything renders at the preset resolution, changes need a restart. Auto turns it on "
           "for powerful discrete GPUs. Applies after a game restart."));
    ImGui::SeparatorText(T("Game effects (after restart)"));
    static const char* lods[] = {T("Highest (-2)"), T("As in the game"), T("Lower (1)"),
                                 T("Lowest (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo(T("Model detail"), lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        Checkbox(T(BbSettings::Effects[e].label), s.effects[e]);
    }
    Hint(T("Effects are switched by game patches at startup (patches/Bloodborne.xml). Motion blur "
           "and dynamic light shadows are noticeably heavy on the GPU."));
    Hint(T("Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left touchpad "
           "/ Tab. Needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from Nexus mod "
           "#253. Right touchpad: Backspace."));

    bool restart = s.controller_layout != s.startup_controller_layout ||
                   s.controller_device != s.startup_controller_device ||
                   s.controller_map != s.startup_controller_map ||
                   s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   s.async_pipelines != s.startup_async_pipelines ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           T("Changes apply after a game restart"));
        if (ImGui::Button(T("Apply and restart the game"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText(T("Other"));
    Checkbox(T("FPS counter in the corner"), s.show_fps);
    Checkbox(T("Experimental async shader compilation"), s.async_pipelines);
    Hint(T("Removes the stutter when something is seen for the first time: a new area, enemy "
           "or attack. The first time only, an enemy or character can show for a moment as a "
           "plain dark figure or not at all, an effect can be missing for a moment, and "
           "loading screens can take a little longer. Applies after a game restart."));

    ImGui::Spacing();
    if (ImGui::Button(T("Close"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(T("Settings are saved in bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    if (prompt_connected) {
        ImGui::TextDisabled(T("Controller: [%s] = OK, [%s] = cancel"), T(FaceLabel(0)), T(FaceLabel(1)));
    }
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text("%.0f FPS  %.1f ms  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                : s.upscaler == BbSettings::UpscalerMetalFx ? "MetalFX"
                                                         : "");
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

void ReleaseController() {
    std::scoped_lock lock{imgui_mutex};
    if (prompt_pad) SDL_CloseGamepad(prompt_pad);
    prompt_pad = nullptr;
    prompt_id = 0;
    prompt_connected = false;
    prompt_device.clear();
    calibrating = false;
    next_controller_scan = 0;
}

void UpdateController() {
    std::scoped_lock lock{imgui_mutex};
    const Uint64 now = SDL_GetTicks();
    if (now < next_controller_scan) return;
    next_controller_scan = now + 500;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    // Match runtime_pad.c: gameplay uses the first available controller.
    const SDL_JoystickID id = ids && count ? ids[0] : 0;
    SDL_free(ids);
    if (id != prompt_id || (prompt_pad && !SDL_GamepadConnected(prompt_pad))) {
        if (prompt_pad) SDL_CloseGamepad(prompt_pad);
        prompt_pad = id ? SDL_OpenGamepad(id) : nullptr;
        prompt_id = prompt_pad ? id : 0;
        prompt_connected = prompt_pad != nullptr;
        calibrating = false;
        l3_down = r3_down = false;
        if (initialized) ImGui::GetIO().ClearInputKeys();
        prompt_device.clear();
        detected_labels = {};
    }
    if (!prompt_pad) return;
    const char* name = SDL_GetGamepadName(prompt_pad);
    prompt_name = name ? name : "Controller";
    prompt_device = BbPrompts::DeviceKey(prompt_pad);
    for (int i = 0; i < 4; ++i) {
        detected_labels[i] = BbPrompts::SdlLabel(SDL_GetGamepadButtonLabel(prompt_pad, SDL_GamepadButton(i)));
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        next_controller_scan = 0;
        if (event.gdevice.which == prompt_id) calibrating = false;
        return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        // Insert, or Cmd+, (macOS settings shortcut: Mac keyboards have no Insert key).
        const bool toggle = event.key.key == SDLK_INSERT ||
                            (event.key.key == SDLK_COMMA && (event.key.mod & SDL_KMOD_GUI));
        if (calibrating && down && event.key.key == SDLK_ESCAPE) {
            calibrating = false;
            return true;
        }
        if (down && !event.key.repeat && (toggle || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(toggle ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (event.gbutton.which != prompt_id) return false;
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (calibrating) {
            if (down && button == SDL_GAMEPAD_BUTTON_START) {
                calibrating = false;
                return true;
            }
            if (down) calibration.Press(button);
            else calibration.Release(button);
            if (calibration.Complete()) {
                auto& s = BbSettings::Get();
                s.controller_map = calibration.map;
                s.controller_device = prompt_device;
                dirty = true;
                calibrating = false;
            }
            return true;
        }
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || BbSettings::Get().show_fps);
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
