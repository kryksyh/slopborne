// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "filter") {
        v.retro_filter = 0;
        for (int fi = 0; fi < BbFilters::Count; ++fi) {
            if (value == BbFilters::All[fi].key) {
                v.retro_filter = fi + 1;
            }
        }
    } else if (key.starts_with("filter.")) {
        // filter.<filter key>.<knob key>
        const auto dot = key.find('.', 7);
        const std::string filter_key = key.substr(7, dot - 7);
        const std::string knob = dot == std::string::npos ? "" : key.substr(dot + 1);
        for (int fi = 0; fi < BbFilters::Count; ++fi) {
            const auto& filter = BbFilters::All[fi];
            if (filter_key != filter.key) continue;
            for (int k = 0; k < filter.param_count; ++k) {
                const auto& param = filter.params[k];
                if (knob == param.key) {
                    v.filter_params[fi][k] = std::clamp(f, param.min, param.max);
                }
            }
        }
    } else if (key == "controller_layout") {
        v.controller_layout = std::clamp(i, 0, int(BbPrompts::LayoutCount) - 1);
    } else if (key == "controller_device") {
        v.controller_device = value;
    } else if (key == "online") {
        v.online = i != 0;
    } else if (key == "online_npid") {
        v.online_npid = value;
    } else if (key == "online_password") {
        v.online_password = value;
    } else if (key == "online_server") {
        v.online_server = value;
    } else if (key == "controller_map") {
        v.controller_map = BbPrompts::ParseMap(value);
    } else if (key == "async_pipelines") {
        v.async_pipelines = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "frame_limit") {
        v.frame_limit = std::clamp(i, 30, 120); // below 30 the game plays in slow motion
    } else if (key == "render_res") {
        for (int r = 0; r < RenderResCount; ++r) {
            if (value == std::to_string(RenderWidths[r]) + "x" + std::to_string(RenderHeights[r])) {
                v.render_res = r;
            }
        }
    } else if (key == "native_upscaler") {
        v.native_upscaler = -1;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.native_upscaler = u;
            }
        }
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

} // namespace BbSettings

float BbFilters::ActiveOwnKnob(int own, int k, float fallback) {
    const auto& v = BbSettings::Get();
    const int f = v.retro_filter - 1;
    if (f < 0 || f >= Count || All[f].own != own || k >= All[f].param_count) {
        return fallback;
    }
    return v.filter_params[f][k];
}

namespace BbSettings {

void ResetFilterParams(int only) {
    auto& v = Get();
    for (int f = 0; f < BbFilters::Count; ++f) {
        if (only >= 0 && f != only) continue;
        for (int k = 0; k < BbFilters::All[f].param_count; ++k) {
            v.filter_params[f][k] = BbFilters::All[f].params[k].def;
        }
    }
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    ResetFilterParams();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_SHOW_FPS", "show_fps"},              {"BB_FILTER", "filter"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_frame_limit = v.frame_limit;
    // The present thread reads BB_FPS_LIMIT (EmulatorSettings::GetFrameLimit); an explicit one
    // (benchmarks, the launcher's --fps) wins over the menu's choice. One written from the menu
    // is marked, so the in-game restart (which inherits this environment) takes the new choice.
    const char* limit_env = std::getenv("BB_FPS_LIMIT");
    const char* from_menu = std::getenv("BB_FPS_LIMIT_FROM_MENU");
    if (!limit_env || (from_menu && from_menu[0] == '1')) {
        const std::string limit = std::to_string(v.frame_limit.load());
#ifdef _WIN32
        _putenv_s("BB_FPS_LIMIT", limit.c_str());
        _putenv_s("BB_FPS_LIMIT_FROM_MENU", "1");
#else
        setenv("BB_FPS_LIMIT", limit.c_str(), 1);
        setenv("BB_FPS_LIMIT_FROM_MENU", "1", 1);
#endif
    }
    if (const char* env = std::getenv("BB_ASYNC_PIPELINES")) {
        v.async_pipelines = env[0] != '0';
    }
    v.startup_async_pipelines = v.async_pipelines;
    v.startup_output_res = v.output_res;
    v.startup_render_res = v.render_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_controller_layout = v.controller_layout;
    v.startup_controller_device = v.controller_device;
    v.startup_online = v.online;
    v.startup_online_npid = v.online_npid;
    v.startup_online_password = v.online_password;
    v.startup_online_server = v.online_server;
    v.startup_controller_map = v.controller_map;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "bbport_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureMetalFxSupport(bool available) {
    auto& v = Get();
    v.metalfx_supported = available;
    if (v.upscaler == UpscalerMetalFx && !available) {
        std::printf("Upscaler: MetalFX unavailable; falling back to FSR 3.1\n");
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    // At the 1080p output the scene size is the menu's render resolution (patched at start):
    // the live presets are only for other outputs.
    return FixedRenderSession() ? v.startup_preset :
        (v.upscaler == UpscalerTaa || v.output_res == OutputDefault) ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    if (v.render_res != v.startup_render_res) {
        return true;
    }
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        ((v.preset != v.startup_preset && v.render_res == RenderNative) ||
         v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void SelectRenderRes(int render_res) {
    auto& v = Get();
    if (render_res == v.render_res) {
        return;
    }
    if (render_res == RenderNative) {
        if (v.native_upscaler >= 0) {
            v.upscaler = v.native_upscaler.load();
        }
        v.native_upscaler = -1;
    } else {
        if (v.render_res == RenderNative) {
            v.native_upscaler = v.upscaler.load();
        }
        const int u = v.upscaler;
        if (!IsSpatial(u) && (v.metalfx_supported || u == UpscalerOff || u == UpscalerTaa)) {
            v.upscaler = v.metalfx_supported ? UpscalerMetalFx : UpscalerFsr3;
        }
    }
    v.render_res = render_res;
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "filter=%s\n",
                 v.retro_filter > 0 ? BbFilters::All[v.retro_filter - 1].key : "off");
    for (int f = 0; f < BbFilters::Count; ++f) {
        const auto& filter = BbFilters::All[f];
        for (int k = 0; k < filter.param_count; ++k) {
            const float value = v.filter_params[f][k];
            if (value != filter.params[k].def) {
                std::fprintf(file, "filter.%s.%s=%g\n", filter.key, filter.params[k].key, value);
            }
        }
    }
    std::fprintf(file, "frame_limit=%d\n", v.frame_limit.load());
    // Read by run.py at start (BB_ONLINE, BB_SHADNET_*).
    std::fprintf(file, "online=%d\nonline_npid=%s\nonline_password=%s\nonline_server=%s\n",
                 int(v.online.load()), v.online_npid.c_str(), v.online_password.c_str(),
                 v.online_server.c_str());
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\nrender_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res],
                 RenderWidths[v.render_res], RenderHeights[v.render_res]);
    if (v.native_upscaler >= 0) {
        std::fprintf(file, "native_upscaler=%s\n", UpscalerName(v.native_upscaler));
    }
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fprintf(file, "async_pipelines=%d\n", int(v.async_pipelines.load()));
    std::fprintf(file, "controller_layout=%d\ncontroller_device=%s\n"
                      "controller_map=%d%d%d%d\n", v.controller_layout.load(),
                 v.controller_device.c_str(),
                 v.controller_map[0], v.controller_map[1], v.controller_map[2], v.controller_map[3]);
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss", "metalfx", "stretch", "nearest", "fsr1", "metalfx_spatial"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
