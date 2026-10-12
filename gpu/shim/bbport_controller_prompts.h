// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <string_view>

namespace BbPrompts {
enum Layout { Auto, PlayStation, Xbox, Nintendo, Positions, LayoutCount };
// Order matches SDL's SOUTH, EAST, WEST, NORTH. Values map logical inputs to
// physical positions; they change labels only, never gameplay bindings.
using FaceMap = std::array<int, 4>;
inline constexpr FaceMap Identity{0, 1, 2, 3};
inline constexpr const char* PositionNames[] = {"Bottom", "Right", "Left", "Top"};
inline constexpr const char* PsNames[] = {"Cross", "Circle", "Square", "Triangle"};

inline bool ValidMap(const FaceMap& map) {
    unsigned seen = 0;
    for (int p : map) {
        if (p < 0 || p >= 4 || (seen & (1u << p))) return false;
        seen |= 1u << p;
    }
    return seen == 15;
}

inline FaceMap ParseMap(std::string_view text) {
    if (text.size() != 4) return Identity;
    FaceMap map{};
    for (int i = 0; i < 4; ++i) map[i] = text[i] - '0';
    return ValidMap(map) ? map : Identity;
}

inline const char* Label(int layout, int logical, const FaceMap& map,
                         const std::array<const char*, 4>& detected) {
    if (logical < 0 || logical >= 4) return "?";
    const int physical = ValidMap(map) ? map[logical] : logical;
    static constexpr const char* xbox[] = {"A", "B", "X", "Y"};
    static constexpr const char* nintendo[] = {"B", "A", "Y", "X"};
    switch (layout) {
    case PlayStation: return PsNames[physical];
    case Xbox: return xbox[physical];
    case Nintendo: return nintendo[physical];
    case Positions: return PositionNames[physical];
    default: return detected[physical] ? detected[physical] : PositionNames[physical];
    }
}

// A complete permutation is published only after all four presses AND releases.
// Partial/duplicate input never changes the saved mapping.
struct Calibration {
    FaceMap map = Identity;
    int step = 0;
    int held = -1;
    unsigned seen = 0;
    bool error = false;

    void Press(int logical) {
        if (step == 4 || held >= 0) return;
        if (logical < 0 || logical >= 4 || (seen & (1u << logical))) {
            error = true;
            return;
        }
        error = false;
        map[logical] = step;
        seen |= 1u << logical;
        held = logical;
    }
    void Release(int logical) {
        if (held >= 0 && logical == held) {
            held = -1;
            ++step;
        }
    }
    bool Complete() const { return step == 4 && ValidMap(map); }
};
} // namespace BbPrompts
