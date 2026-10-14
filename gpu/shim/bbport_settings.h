// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <array>
#include "bbport_filters.h"

#include <atomic>
#include <string>
#include "bbport_controller_prompts.h"

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerMetalFx = 6, UpscalerStretch = 7,
                      UpscalerNearest = 8, UpscalerFsr1 = 9,
                      UpscalerMetalFxSpatial = 10, UpscalerCount };
/// Single-frame scaling of a reduced scene (no history, no smearing): the plain path.
inline bool IsSpatial(int upscaler) {
    return upscaler == UpscalerStretch || upscaler == UpscalerNearest || upscaler == UpscalerFsr1 ||
           upscaler == UpscalerMetalFxSpatial;
}
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", true},
    {"effect_dof", "Depth of field (DoF)", true},
    {"effect_motion_blur", "Motion blur", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", true},
    {"effect_game_aa", "Game's own anti-aliasing", true},
    {"effect_dynamic_shadows", "Dynamic light shadows", true},
    {"effect_ssr", "SSR reflections (not in the original game)", false},
    {"skip_intro", "Skip intro videos", false},
    {"debug_camera", "Free camera (Cross + L3)", false},
    {"debug_menu", "Debug menu (needs font files)", false},
    {"cheat_no_death", "Cheat: never die (HP stays at 1)", false},
    {"cheat_stealth", "Cheat: enemies don't see you", false},
    {"cheat_silent", "Cheat: enemies don't hear you", false},
    {"cheat_rally_no_decay", "Cheat: Rally never fades", false},
    {"cheat_enemy_control", "Cheat: control an enemy (R3 / L3)", false},
    {"tweak_no_rally", "No Rally (HP regain)", false},
    {"tweak_camera_distance", "Camera farther away", false},
    {"tweak_no_camera_rotation", "No camera auto-rotation", false},
    {"tweak_easy_run", "Run with less stick tilt", false},
    {"tweak_ragdoll", "Dark Souls ragdoll physics", false},
    {"tweak_fast_deflate", "No hitch every ~50 s (faster save compression)", true},
    {"tweak_quick_reentry", "Quick loading after death and lamp travel", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size
/// Scene sizes (render_res), offered when smaller than the output: patched at start
/// (patches.py), and an upscaler fills the output. RenderNative is the output's own size.
/// The smallest is 384x216, not 428x240: that width broke the scene after reloads (2026-10-14).
inline constexpr int RenderWidths[] = {3840, 2560, 1920, 1600, 1280, 960, 640, 384};
inline constexpr int RenderHeights[] = {2160, 1440, 1080, 900, 720, 540, 360, 216};
inline constexpr int RenderResCount = 8;
inline constexpr int RenderNative = -1;
/// The scene size is smaller than the output (all sizes are 16:9).
inline bool RenderFits(int render_res, int output_res) {
    return render_res >= 0 && render_res < RenderResCount &&
           RenderWidths[render_res] < OutputWidths[output_res];
}


struct Values {
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// bbport: the Filters tab: 0 off, else BbFilters::All[retro_filter - 1]; knobs per filter.
    std::atomic<int> retro_filter{0};
    std::array<std::array<std::atomic<float>, BbFilters::MaxParams>, BbFilters::Count> filter_params{};
    std::atomic<int> controller_layout{BbPrompts::Auto};
    // Frontend-only, protected by the overlay mutex after startup Load().
    std::string controller_device;
    /// Online play through a shadNet server: read by the launcher at start (run.py).
    std::atomic<bool> online{false};
    std::string online_npid, online_password, online_server;
    BbPrompts::FaceMap controller_map = BbPrompts::Identity;
    int startup_controller_layout = BbPrompts::Auto;
    std::string startup_controller_device;
    BbPrompts::FaceMap startup_controller_map = BbPrompts::Identity;
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    /// Frames per second at most (30, 40, 60, 120); read at start. BB_FPS_LIMIT overrides.
    std::atomic<int> frame_limit{30};
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    std::atomic<int> render_res{RenderNative}; ///< index into RenderWidths; on restart
    /// The upscaler chosen before a reduced render_res, restored at native; -1 none.
    std::atomic<int> native_upscaler{-1};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Driver pipeline compiles in the background: a new object's first draws are skipped for a
    /// moment instead of the frame waiting (PipelineCache; read at start).
    std::atomic<bool> async_pipelines{true};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (bbport_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
    /// Apple MetalFX temporal scaler (macOS on Apple GPUs).
    std::atomic<bool> metalfx_supported{false};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_frame_limit = 30;
    bool startup_online = false;
    std::string startup_online_npid, startup_online_password, startup_online_server;
    int startup_output_res = OutputDefault;
    int startup_render_res = RenderNative;
    int startup_live_resolution = 0;
    bool startup_async_pipelines = true;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// After device creation: MetalFX availability; a MetalFX setting falls back to FSR 3.1 without it.
void ConfigureMetalFxSupport(bool available);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Menu choice of render_res: a reduced size needs an upscaler (MetalFX where supported); the
/// upscaler used before it comes back at native.
void SelectRenderRes(int render_res);
/// Knobs back to their defaults: one filter (index into BbFilters::All) or all (-1).
void ResetFilterParams(int only = -1);
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
