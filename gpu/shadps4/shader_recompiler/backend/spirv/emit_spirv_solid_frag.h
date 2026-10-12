// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/info.h"

namespace Shader::Backend::SPIRV {

/// bbport: a fragment shader writing a constant color to each listed output location (the
/// stand-in a pipeline draws with while its own compiles).
struct SolidOutput {
    u32 location;
    std::array<float, 4> color;
};

/// With `textured`, the output at `textured_location` samples the shader's own texture
/// (Info::StandIn::Target) instead of a constant; `alpha_test` drops texels with alpha under 0.5.
[[nodiscard]] std::vector<u32> EmitSolidFragmentShader(
    std::span<const SolidOutput> outputs, const Info::StandIn::Target* textured = nullptr,
    u32 textured_location = 0, bool alpha_test = false);

} // namespace Shader::Backend::SPIRV
