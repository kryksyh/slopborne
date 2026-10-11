// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <vector>
#include "common/types.h"

namespace Shader::Backend::SPIRV {

/// bbport: a fragment shader writing a constant color to each listed output location (the
/// stand-in a pipeline draws with while its own compiles).
struct SolidOutput {
    u32 location;
    std::array<float, 4> color;
};
[[nodiscard]] std::vector<u32> EmitSolidFragmentShader(std::span<const SolidOutput> outputs);

} // namespace Shader::Backend::SPIRV
