// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"

namespace Vulkan {

// Separate from native graphics/compute shader caches: the execution model and
// appended descriptors differ even when the guest program hash is identical.
struct GeometryKernel {
    Shader::Info info;
    Shader::StageSpecialization specialization;
    Shader::Backend::SPIRV::GeometryComputeLayout layout;
    vk::UniqueShaderModule module;
    std::unique_ptr<ComputePipeline> pipeline;
};

struct GeometryReplayStages {
    const GeometryKernel* es;
    const GeometryKernel* gs;
    Shader::Info replay_info;
    vk::UniqueShaderModule replay_module;
    u32 input_binding;
    u32 max_vertices;
    bool last_provoking_vertex;
    bool remap_negative_depth;
    u32 flat_corner_mask; ///< replay outputs flat corners of these parameters
};

} // namespace Vulkan
