// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the Filters tab: every picture filter with its knobs. The port's own depth-aware
// filters first, then the libretro shaders (generated, host_shaders/libretro/ATTRIBUTION.md).
#pragma once

#include <iterator>

namespace BbFilters {

inline constexpr int MaxParams = 24;

struct Param {
    const char* key;
    const char* label;
    float min, max, def, step;
    const char* choices = nullptr; ///< "A|B|C": an enum shown as a combo (value = index)
    const char* when = nullptr;    ///< "key=value": only used while that knob has that value
};

/// The port's own shaders (vk_temporal_upscaler.cpp picks the SPIR-V by this id).
enum Own : int {
    OwnNone = -1, OwnCel, OwnObraDinn, OwnSinCity, OwnEngraving, OwnPencil, OwnOil,
    OwnWatercolor, OwnAscii, OwnDatamosh, OwnVision, OwnLines, OwnTiltShift, OwnPs1, OwnCount
};

struct Filter {
    const char* key;
    const char* name;
    const char* group;
    const char* about;
    const Param* params;
    int param_count;
    int own = OwnNone;
};

#include "bbport_filters_libretro.inc"

inline constexpr Param CelParams[] = {
    {"bands", "Tone bands", 2, 6, 3, 1},
    {"adapt", "Adapt to local light", 0, 1, 0.5f, 0.05f},
    {"smooth", "Flatten texture detail", 0, 1, 1, 1},
    {"outline", "Outline strength", 0, 1, 0.85f, 0.05f},
    {"edge", "Outline sensitivity", 0.25f, 4, 1, 0.05f},
    {"width", "Outline width", 1, 3, 1, 1},
    {"saturation", "Saturation", 0, 2, 1, 0.05f},
    {"colors", "Color levels", 0, 8, 0, 1},
};
inline constexpr Param RotoscopeDefaults[] = {
    {"bands", "Tone bands", 2, 6, 4, 1},
    {"adapt", "Adapt to local light", 0, 1, 0.35f, 0.05f},
    {"smooth", "Flatten texture detail", 0, 1, 1, 1},
    {"outline", "Outline strength", 0, 1, 1, 0.05f},
    {"edge", "Outline sensitivity", 0.25f, 4, 1.4f, 0.05f},
    {"width", "Outline width", 1, 3, 2, 1},
    {"saturation", "Saturation", 0, 2, 1.25f, 0.05f},
    {"colors", "Color levels", 0, 8, 4, 1},
};
inline constexpr const char* InkPalettes =
    "Macintosh|IBM 5151 green|Zenith amber|Commodore 1084|LCD|Black on white|Sepia";
inline constexpr Param ObraParams[] = {
    {"palette", "Palette", 0, 6, 0, 1, InkPalettes},
    {"ink", "Ink below", 0, 1, 0.38f, 0.01f},
    {"paper", "Paper above", 0, 1, 0.55f, 0.01f},
    {"pattern", "Dither pattern", 0, 2, 0, 1, "Bayer 8x8|Bayer 4x4|Noise"},
    {"stable", "Pattern follows the camera", 0, 1, 1, 1},
    {"outlines", "Outlines", 0, 1, 1, 1},
    {"edge", "Outline sensitivity", 0.25f, 4, 1, 0.05f, nullptr, "outlines=1"},
    {"adapt", "Adapt to local light", 0, 1, 0.5f, 0.05f},
};
inline constexpr Param SinCityParams[] = {
    {"threshold", "Black / white threshold", 0, 1, 0.42f, 0.01f},
    {"softness", "Gray range", 0, 0.5f, 0.08f, 0.01f},
    {"red", "Keep red", 0, 1, 1, 0.05f},
    {"tolerance", "Red tolerance", 0.05f, 0.6f, 0.38f, 0.01f},
    {"outlines", "Outlines", 0, 1, 1, 1},
    {"grain", "Film grain", 0, 0.3f, 0.06f, 0.01f},
};
inline constexpr Param EngravingParams[] = {
    {"palette", "Ink", 0, 3, 0, 1, "Sepia on parchment|Black on paper|Banknote green|White on black"},
    {"spacing", "Line spacing", 2, 10, 4, 0.5f},
    {"angle", "Line angle", 0, 180, 35, 1},
    {"cross", "Cross-hatch in shadows", 0, 1, 1, 1},
    {"stable", "Lines follow the camera", 0, 1, 1, 1},
    {"outlines", "Outlines", 0, 1, 1, 1},
    {"contrast", "Contrast", 0.5f, 2, 1.2f, 0.05f},
};
inline constexpr Param PencilParams[] = {
    {"spacing", "Stroke spacing", 2, 8, 3.5f, 0.5f},
    {"layers", "Hatch layers", 1, 4, 3, 1},
    {"surface", "Strokes follow surfaces", 0, 1, 1, 1},
    {"grain", "Paper grain", 0, 1, 0.5f, 0.05f},
    {"outlines", "Outlines", 0, 1, 1, 1},
    {"tint", "Colored pencil", 0, 1, 0, 0.05f},
};
inline constexpr Param OilParams[] = {
    {"radius", "Brush size", 2, 6, 3, 1},
    {"saturation", "Saturation", 0, 2, 1.2f, 0.05f},
    {"canvas", "Canvas texture", 0, 1, 0.4f, 0.05f},
    {"outlines", "Outlines", 0, 1, 0, 1},
};
inline constexpr Param WatercolorParams[] = {
    {"softness", "Softness", 1, 4, 2, 0.5f},
    {"edges", "Pigment edges", 0, 1, 0.6f, 0.05f},
    {"grain", "Paper grain", 0, 1, 0.5f, 0.05f},
    {"saturation", "Saturation", 0, 2, 1.1f, 0.05f},
    {"bleed", "Color bleed", 0, 1, 0.4f, 0.05f},
};
inline constexpr Param AsciiParams[] = {
    {"cell", "Character size", 6, 24, 10, 1},
    {"color", "Colors", 0, 3, 0, 1, "Green terminal|Amber terminal|Scene colors|White"},
    {"charset", "Characters", 0, 1, 0, 1, " .:-=+*#%@|Blocks"},
    {"background", "Dim scene behind", 0, 1, 0, 0.05f},
};
inline constexpr Param DatamoshParams[] = {
    {"smear", "Smear along motion", 0, 3, 1.2f, 0.05f},
    {"keyframe", "Seconds between clean frames", 0.2f, 8, 2.5f, 0.1f},
    {"blocks", "Block size", 1, 16, 8, 1},
    {"glitch", "Color glitch", 0, 1, 0.3f, 0.05f},
};
inline constexpr Param VisionParams[] = {
    {"mode", "Mode", 0, 2, 0, 1, "Thermal|Night vision|Predator"},
    {"noise", "Sensor noise", 0, 0.4f, 0.08f, 0.01f},
    {"depth", "Distance falloff", 0, 1, 0.5f, 0.05f},
    {"scope", "Scope vignette", 0, 1, 1, 1},
    {"scanlines", "Scanlines", 0, 1, 1, 1},
};
inline constexpr Param LinesParams[] = {
    {"style", "Style", 0, 3, 0, 1, "Tron|Blueprint|Chalkboard|Ink sketch"},
    {"width", "Line width", 1, 3, 1, 1},
    {"glow", "Glow", 0, 1, 0.6f, 0.05f},
    {"scene", "Show the scene", 0, 1, 0.15f, 0.05f},
    {"edge", "Line sensitivity", 0.25f, 4, 1.5f, 0.05f},
};
inline constexpr Param TiltShiftParams[] = {
    {"auto", "Focus on the screen center", 0, 1, 1, 1},
    {"focus", "Focus distance (m)", 1, 60, 8, 0.5f, nullptr, "auto=0"},
    {"band", "Sharp band", 0.05f, 1, 0.25f, 0.01f},
    {"blur", "Blur", 0, 8, 4, 0.25f},
    {"saturation", "Saturation", 0, 2, 1.35f, 0.05f},
};

inline constexpr Param Ps1Params[] = {
    {"jitter", "Vertex jitter", 0, 480, 240, 30},
    {"nearest", "Unfiltered textures", 0, 1, 1, 1},
    {"colors", "15-bit color", 0, 1, 1, 1},
    {"dither", "PS1 dither", 0, 1, 1, 1},
    {"hard", "Hard pixels", 0, 1, 1, 1},
};

/// The knob `k` of the selected filter when it is the `own` filter, else `fallback`.
float ActiveOwnKnob(int own, int k, float fallback);

inline constexpr Filter All[] = {
    {"cel", "Cel shading", "Filters", "Flat tone bands with depth outlines.", CelParams,
     int(std::size(CelParams)), OwnCel},
    {"rotoscope", "Rotoscope", "Filters", "Posterized colors and thick outlines, like A Scanner Darkly.",
     RotoscopeDefaults, int(std::size(RotoscopeDefaults)), OwnCel},
    {"obra_dinn", "Obra Dinn", "Filters",
     "1-bit look after Lucas Pope's Return of the Obra Dinn.", ObraParams,
     int(std::size(ObraParams)), OwnObraDinn},
    {"sin_city", "Sin City", "Filters", "Black and white; only blood stays red.", SinCityParams,
     int(std::size(SinCityParams)), OwnSinCity},
    {"engraving", "Victorian engraving", "Filters", "Banknote-style line hatching.",
     EngravingParams, int(std::size(EngravingParams)), OwnEngraving},
    {"pencil", "Pencil sketch", "Filters", "Pencil cross-hatching.", PencilParams,
     int(std::size(PencilParams)), OwnPencil},
    {"oil", "Oil paint", "Filters", "Oil paint brush strokes.", OilParams,
     int(std::size(OilParams)), OwnOil},
    {"watercolor", "Watercolor", "Filters", "Watercolor washes on paper.", WatercolorParams,
     int(std::size(WatercolorParams)), OwnWatercolor},
    {"ascii", "ASCII art", "Filters", "Characters by brightness.", AsciiParams,
     int(std::size(AsciiParams)), OwnAscii},
    {"datamosh", "Datamosh", "Filters", "Frames smeared like a broken video codec.",
     DatamoshParams, int(std::size(DatamoshParams)), OwnDatamosh},
    {"vision", "Thermal / night vision", "Filters", "Thermal or night vision.", VisionParams,
     int(std::size(VisionParams)), OwnVision},
    {"lines", "Tron / Blueprint", "Filters", "Glowing outlines only.", LinesParams,
     int(std::size(LinesParams)), OwnLines},
    {"ps1", "PlayStation", "Filters",
     "For better results, set the lowest render resolution (216p) and the lowest level of "
     "detail on the Display tab.", Ps1Params, int(std::size(Ps1Params)), OwnPs1},
    {"tilt_shift", "Tilt-shift miniature", "Filters", "Depth blur that turns Yharnam into a miniature.",
     TiltShiftParams, int(std::size(TiltShiftParams)), OwnTiltShift},
    BB_LIBRETRO_FILTERS
};
inline constexpr int Count = int(std::size(All));
/// The port's own filters come first in All, the libretro ones after them in RetroShaders order.
inline constexpr int OwnEntries = [] {
    int n = 0;
    for (const auto& f : All) n += f.own != OwnNone;
    return n;
}();

} // namespace BbFilters
