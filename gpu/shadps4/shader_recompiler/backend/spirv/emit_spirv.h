// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Backend::SPIRV {

// Experimental point-GS compute lowering. Input: count, points_per_instance
// (0 = one instance), 2 reserved DWORDs, then packed ES attributes per point. Output per point: vertex count, overflow flag,
// then max_vertices records of [strip_start_vertex, position.xyzw, param0.xyzw, ...].
// The caller owns descriptor binding, allocation and compute-to-draw barriers.
// With HwStage::Export/SwStage::Vertex, input is an 8-DWORD draw header:
// vertex_count, instance_count, base_vertex, base_instance, indexed, 3 reserved,
// followed by vertex_count u32 indices when indexed. Output is the GS input above.
// The caller dispatches at least one workgroup, covering the bounded total point count.
struct GeometryComputeLayout {
    u32 input_binding{};
    u32 output_binding{};
    u32 attribute_dwords{};
    u32 vertex_stride_dwords{};
    u32 primitive_stride_dwords{};
};

/// Locations of the flat corner inputs that replace per-vertex fragment inputs
/// (Profile::lower_per_vertex_to_flat_corners): corner 0 keeps the attribute's location.
constexpr u32 FlatCornerLocationStride = 8;
constexpr u32 FlatCornerLocation(u32 location, u32 corner) {
    return corner == 0 ? location : FlatCornerLocationStride * (corner + 1) + location;
}

// Triangle-strip replay: draw (max_vertices - 2) * 3 vertices per point instance.
// Descriptor set 0/input_binding holds the expanded compute buffer. Invalid/incomplete
// triangles are clipped, and winding restarts at every strip boundary. The caller
// must match last_provoking_vertex to the replay graphics pipeline state.
// Optional capture_binding writes valid vertex attributes for GPU diagnostics;
// it requires vertexPipelineStoresAndAtomics and is absent from normal replay.
[[nodiscard]] std::vector<u32> EmitGeometryReplayVertex(const Profile& profile,
                                                       const GeometryComputeLayout& geometry,
                                                       u32 max_vertices, u32 capture_binding = ~0u,
                                                       bool last_provoking_vertex = true,
                                                       u32 input_binding = 0,
                                                       bool native_last_provoking = true,
                                                       bool remap_negative_depth = false,
                                                       u32 flat_corner_mask = 0);

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                                         const IR::Program& program, Bindings& binding,
                                         GeometryComputeLayout* geometry = nullptr);

} // namespace Shader::Backend::SPIRV
