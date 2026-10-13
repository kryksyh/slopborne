// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <thread>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/backend/spirv/emit_spirv_solid_frag.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

/// bbport: the create-info of a graphics pipeline and everything it points to.
struct GraphicsPipeline::CreateInfo {
    VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes;
    VertexInputs<vk::VertexInputBindingDescription> vertex_bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    vk::PipelineMultisampleStateCreateInfo multisampling;
    vk::PipelineVertexInputDivisorStateCreateInfo divisor_state;
    vk::PipelineVertexInputStateCreateInfo vertex_input_info;
    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    vk::PipelineTessellationStateCreateInfo tessellation_state;
    vk::StructureChain<vk::PipelineRasterizationStateCreateInfo,
                       vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT,
                       vk::PipelineRasterizationDepthClipStateCreateInfoEXT>
        raster_chain;
    vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control;
    vk::PipelineViewportStateCreateInfo viewport_info;
    boost::container::static_vector<vk::DynamicState, 32> dynamic_states;
    vk::PipelineDynamicStateCreateInfo dynamic_info;
    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages> shader_stages;
    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci;
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats;
    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    vk::AttachmentSampleCountInfoAMD mixed_samples;
    vk::PipelineRenderingCreateInfo pipeline_rendering_ci;
    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    vk::PipelineColorBlendStateCreateInfo color_blending;
    vk::PipelineDepthStencilStateCreateInfo depth_stencil_info;
    vk::GraphicsPipelineCreateInfo pipeline_info;
};

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading,
    const GeometryReplayStages* geometry_, AsyncCompiler* async, u32 tess_fused_vertices)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, geometry{geometry_},
      tess_fused{tess_fused_vertices}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    // bbport: everything the driver compile reads lives in one allocation, so the compile can run
    // on the AsyncCompiler after this constructor returned.
    auto ci = std::make_unique<CreateInfo>();
    ci->vertex_attributes = sdata.vertex_attributes;
    ci->vertex_bindings = sdata.vertex_bindings;
    ci->divisors = sdata.divisors;

    ci->divisor_state = vk::PipelineVertexInputDivisorStateCreateInfo{
        .vertexBindingDivisorCount = static_cast<u32>(ci->divisors.size()),
        .pVertexBindingDivisors = ci->divisors.data(),
    };

    ci->vertex_input_info = vk::PipelineVertexInputStateCreateInfo{
        .pNext = ci->divisors.empty() ? nullptr : &ci->divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(ci->vertex_bindings.size()),
        .pVertexBindingDescriptions = ci->vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(ci->vertex_attributes.size()),
        .pVertexAttributeDescriptions = ci->vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    ci->input_assembly = vk::PipelineInputAssemblyStateCreateInfo{
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    ci->tessellation_state = vk::PipelineTessellationStateCreateInfo{
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    ci->raster_chain = vk::StructureChain{
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        ci->raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        ci->raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }
    ci->multisampling = sdata.multisampling;

    ci->clip_control = vk::PipelineViewportDepthClipControlCreateInfoEXT{
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    ci->viewport_info = vk::PipelineViewportStateCreateInfo{
        .pNext = instance.IsDepthClipControlSupported() ? &ci->clip_control : nullptr,
    };

    ci->dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (instance.IsDepthBoundsSupported()) {
        ci->dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        ci->dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        ci->dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        ci->dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!ci->vertex_bindings.empty()) {
        ci->dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    ci->dynamic_info = vk::PipelineDynamicStateCreateInfo{
        .dynamicStateCount = static_cast<u32>(ci->dynamic_states.size()),
        .pDynamicStates = ci->dynamic_states.data(),
    };

    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage] && !tess_fused) {
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
        }
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage] && !tess_fused) {
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, fs_info);
        }
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        if (!preloading) {
            const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

            sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        }
        ci->shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    ci->subgroup_size_ci = vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo{
        .requiredSubgroupSize = 64,
    };
    for (auto& stage : ci->shader_stages) {
        stage.pNext = instance.IsSubgroupSize64Supported() ? &ci->subgroup_size_ci : nullptr;
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        ci->color_formats[i] = color_format;
    }

    std::ranges::transform(key.color_samples, ci->color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    ci->mixed_samples = vk::AttachmentSampleCountInfoAMD{
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = ci->color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    ci->pipeline_rendering_ci = vk::PipelineRenderingCreateInfo{
        .pNext = instance.IsMixedDepthSamplesSupported() ? &ci->mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = ci->color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        ci->attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
            ci->attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            ci->attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            ci->attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            ci->attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    ci->color_blending = vk::PipelineColorBlendStateCreateInfo{
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = ci->attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it. MoltenVK still decides from the static
    // state whether a pipeline without color ci->attachments rasterizes: with tests off it drops
    // stencil-only draws (HUD bar masks), so the tests the ci->attachments allow are left on.
    const bool has_depth = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid;
    const bool has_stencil = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid;
    ci->depth_stencil_info = vk::PipelineDepthStencilStateCreateInfo{
        .depthTestEnable = has_depth,
        .depthWriteEnable = has_depth,
        .depthCompareOp = vk::CompareOp::eAlways,
        .stencilTestEnable = has_stencil,
    };

    ci->pipeline_info = vk::GraphicsPipelineCreateInfo{
        .pNext = &ci->pipeline_rendering_ci,
        .stageCount = static_cast<u32>(ci->shader_stages.size()),
        .pStages = ci->shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &ci->vertex_input_info : nullptr,
        .pInputAssemblyState = &ci->input_assembly,
        .pTessellationState = &ci->tessellation_state,
        .pViewportState = &ci->viewport_info,
        .pRasterizationState = &ci->raster_chain.get(),
        .pMultisampleState = &ci->multisampling,
        .pDepthStencilState = &ci->depth_stencil_info,
        .pColorBlendState = &ci->color_blending,
        .pDynamicState = &ci->dynamic_info,
        .layout = *pipeline_layout,
    };

    if (async && !geometry) {
        // Published to draws by IsReady once the worker finished the driver compile.
        ready.store(false, std::memory_order_relaxed);
        pending = std::move(ci);
        pending_cache = pipeline_cache;
        pending_name = debug_str;
        this->async = async;
        queued = std::chrono::steady_clock::now();
        QueueStandIn(*async);
        async->Submit([this] { CompileNow(); });
    } else {
        Create(pipeline_cache, ci->pipeline_info, debug_str, preloading);
    }
}

void GraphicsPipeline::QueueStandIn(AsyncCompiler& async) {
    const auto& ci = *pending;
    const auto fragment = std::ranges::find(ci.shader_stages, vk::ShaderStageFlagBits::eFragment,
                                            &vk::PipelineShaderStageCreateInfo::stage);
    const bool has_geometry = std::ranges::any_of(ci.shader_stages, [](const auto& stage) {
        return stage.stage == vk::ShaderStageFlagBits::eGeometry;
    });
    if (fragment == ci.shader_stages.end() || has_geometry) {
        return; // nothing to replace, or a geometry stage (whose own draws are replayed)
    }
    std::vector<Shader::Backend::SPIRV::SolidOutput> outputs;
    const auto* fs_info = stages[u32(Shader::SwStage::Fragment)];
    const Shader::Info::StandIn::Target* textured = nullptr;
    u32 textured_location = 0;
    for (u32 i = 0; i < key.num_color_attachments; ++i) {
        if (ci.attachments[i].blendEnable) {
            return; // blended effects stay out until their own pipeline is ready
        }
        const auto format = ci.color_formats[i];
        if (format == vk::Format::eUndefined) {
            continue;
        }
        const auto name = vk::to_string(format);
        if (name.find("int") != std::string::npos) {
            return; // integer targets need integer outputs
        }
        // Float targets (emissive, lighting) stay black, so the surface is lit by the scene and
        // does not glow. The game's G-buffer: target 0 normals, 2 base color (sRGB); the
        // constants are scene averages of those, a dark grey elsewhere.
        const bool is_float = name.find("float") != std::string::npos;
        const bool is_srgb = name.find("Srgb") != std::string::npos;
        const bool gbuffer = key.num_color_attachments >= 4;
        std::array<float, 4> color{0.15f, 0.15f, 0.15f, 1.0f};
        if (is_float) {
            color = {0.0f, 0.0f, 0.0f, 1.0f};
        } else if (is_srgb) {
            color = {0.30f, 0.27f, 0.22f, 0.9f};
        } else if (gbuffer && i == 0) {
            color = {0.30f, 0.60f, 0.30f, 0.0f};
        }
        outputs.push_back({i, color});
        // The base color target shows the shader's own texture when the translation found it.
        if (is_srgb && !textured && fs_info && fs_info->stand_in.targets[i].valid) {
            textured = &fs_info->stand_in.targets[i];
            textured_location = i;
        }
    }
    const size_t fragment_index = fragment - ci.shader_stages.begin();
    const auto build = [this, fragment_index](const CreateInfo& ci, vk::ShaderModule module) {
        auto shader_stages = ci.shader_stages;
        shader_stages[fragment_index] = vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = module,
            .pName = "main",
        };
        auto info = ci.pipeline_info;
        info.pStages = shader_stages.data();
        auto [result, pipe] = instance.GetDevice().createGraphicsPipelineUnique(pending_cache, info);
        return result == vk::Result::eSuccess ? std::move(pipe) : vk::UniquePipeline{};
    };
    // The grey stand-in is shared with every pipeline that differs only in the fragment shader:
    // the vertex stages and the render state are the whole key. Under MoltenVK the descriptor
    // set of this pipeline binds the vertex-stage resources at the same Metal indices (the
    // vertex bindings come first and are identical), and the solid fragment shader reads none.
    auto shared_key = key;
    shared_key.stage_hashes[u32(Shader::SwStage::Fragment)] = 0;
    u64 key_hash = XXH3_64bits(&shared_key, sizeof(shared_key));
    for (const auto& stage : ci.shader_stages) {
        if (stage.stage != vk::ShaderStageFlagBits::eFragment) {
            const VkShaderModule module = stage.module;
            key_hash = XXH3_64bits_withSeed(&module, sizeof(module), key_hash);
        }
    }
    bool created = false;
    shared_stand_in = &async.SharedStandInFor(key_hash, created);
    if (created) {
        const auto grey = Shader::Backend::SPIRV::EmitSolidFragmentShader(outputs);
        if (const auto module = async.StandInModule(instance, grey)) {
            async.SubmitFront([this, ci = pending, module, shared = shared_stand_in, build] {
                if (auto pipe = build(*ci, module)) {
                    shared->pipeline = std::move(pipe);
                    shared->ready.store(true, std::memory_order_release);
                    this->async->stats.NoteStandIn(SinceQueuedMs());
                }
            });
        }
    }
    if (!textured) {
        return; // the shared grey stand-in is all this pipeline could show
    }
    const auto spirv = Shader::Backend::SPIRV::EmitSolidFragmentShader(
        outputs, textured, textured_location, fs_info->stand_in.alpha_test);
    stand_in_module = async.StandInModule(instance, spirv);
    if (!stand_in_module) {
        return;
    }
    stand_in_textured = true;
    async.SubmitFront([this, ci = pending, build] {
        if (IsReady()) {
            return;
        }
        if (auto pipe = build(*ci, stand_in_module)) {
            stand_in = std::move(pipe);
            stand_in_ready.store(true, std::memory_order_release);
        }
    });
}

void GraphicsPipeline::CompileNow() {
    if (compile_claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    Create(pending_cache, pending->pipeline_info, pending_name, true);
    pending.reset();
    ready.store(true, std::memory_order_release);
    async->stats.NoteReady(SinceQueuedMs());
}

void GraphicsPipeline::Prioritize() {
    CompileNow();
    while (!IsReady()) {
        std::this_thread::yield();
    }
}

void GraphicsPipeline::Create(vk::PipelineCache pipeline_cache,
                              const vk::GraphicsPipelineCreateInfo& pipeline_info,
                              const std::string& debug_str, bool may_fail) {
    const vk::Device device = instance.GetDevice();
    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    // A preloaded or background compile that fails (an entry from another build or a shipped
    // list that does not fit) is compiled again from the game's shaders when a draw needs it.
    if (pipeline_result != vk::Result::eSuccess && may_fail) {
        LOG_ERROR(Render_Vulkan, "Pipeline {} failed to compile ({}); compiled again when used",
                  debug_str, vk::to_string(pipeline_result));
        failed.store(true, std::memory_order_release);
        return;
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const bool tess_stage = stage->sw_stage == Shader::SwStage::TessellationEval ||
                                stage->sw_stage == Shader::SwStage::TessellationControl;
        const auto stage_bit = tess_fused && tess_stage
                                   ? vk::ShaderStageFlagBits::eVertex
                                   : LogicalStageToStageBit[u32(stage->sw_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage); // See for the comment in compute PL creation
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    // Use the same MoltenVK descriptor workaround as compute pipelines.
    uses_push_descriptors = binding < instance.MaxPushDescriptors() &&
                           instance.GetDriverID() != vk::DriverId::eMoltenvk;
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
