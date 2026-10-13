// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <vector>
#include "common/types.h"

namespace Vulkan {

struct FusedTessellation {
    std::vector<u32> spirv;
    u32 vertices{}; ///< per patch, a triangle list
};

/// bbport: one vertex shader doing the work of a tessellated draw whose patches are single points
/// and whose hull stage stores constant levels giving a fixed quad pattern (two triangles, or a
/// fan around the centre): the local (vertex) stage reads the point, the domain stage runs at the
/// pattern's vertices. Drawn as `vertices` vertices per instance, one instance per point
/// (gl_InstanceIndex is the point's vertex index). Metal has no tessellation: MoltenVK runs every
/// such draw as a compute pass plus a new render pass.
/// Empty when the modules hold something the linker does not handle.
std::optional<FusedTessellation> FuseQuadTessellation(std::span<const u32> local,
                                                      std::span<const u32> hull,
                                                      std::span<const u32> domain);

} // namespace Vulkan
