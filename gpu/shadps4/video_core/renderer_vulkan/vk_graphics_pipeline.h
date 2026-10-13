// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;
struct GeometryReplayStages;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    u32 motion_vectors = 0;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    bool HasNoRasterOutput() const noexcept {
        return num_color_attachments == 0 && motion_vectors == 0 &&
               z_format == AmdGpu::DepthBuffer::ZFormat::Invalid &&
               stencil_format == AmdGpu::DepthBuffer::StencilFormat::Invalid &&
               stage_hashes[u32(Shader::SwStage::Fragment)] == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, const GeometryReplayStages* geometry = nullptr,
                     AsyncCompiler* async = nullptr, u32 tess_fused_vertices = 0);
    ~GraphicsPipeline();

    /// bbport: while the pipeline compiles in the background, opaque draws use a stand-in with
    /// the same vertex stage and a constant color (compiled first: no fragment shader to build).
    bool StandInReady() const noexcept {
        return stand_in_ready.load(std::memory_order_acquire);
    }
    bool SharedStandInReady() const noexcept {
        return shared_stand_in && shared_stand_in->ready.load(std::memory_order_acquire);
    }
    bool AnyStandInReady() const noexcept {
        return StandInReady() || SharedStandInReady();
    }
    bool HasStandIn() const noexcept {
        return static_cast<bool>(stand_in_module) || shared_stand_in;
    }
    bool StandInTextured() const noexcept {
        return stand_in_textured;
    }
    /// The pipeline itself once compiled; until then its own (textured) stand-in, else the
    /// shared one.
    vk::Pipeline Handle() const noexcept {
        if (IsReady() || !AnyStandInReady()) {
            return *pipeline;
        }
        return StandInReady() ? *stand_in : *shared_stand_in->pipeline;
    }

    /// bbport: the background or preloaded compile failed; the cache compiles it again.
    bool Failed() const noexcept {
        return failed.load(std::memory_order_acquire);
    }

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GeometryReplayStages* Geometry() const noexcept { return geometry; }

    /// bbport: quad tessellation drawn by one vertex shader (vk_tess_fusion.h): the hull and
    /// domain stages' infos bind their resources, there is no hull or domain shader stage.
    /// Vertices drawn per patch, 0 when not fused.
    u32 TessFusedVertices() const noexcept { return tess_fused; }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    void Create(vk::PipelineCache pipeline_cache, const vk::GraphicsPipelineCreateInfo& pipeline_info,
                const std::string& debug_str, bool may_fail);
    void CompileNow();
    /// A draw that cannot go without this pipeline: compiles it on the calling thread unless a
    /// worker already started it (then waits for that), instead of waiting behind the queue.
    void Prioritize();
    void QueueStandIn(AsyncCompiler& async);

    /// bbport: create-info of a compile handed to the AsyncCompiler, until it ran.
    struct CreateInfo;
    std::shared_ptr<CreateInfo> pending;
    vk::ShaderModule stand_in_module; ///< from AsyncCompiler::StandInModule
    bool stand_in_textured{};
    SharedStandIn* shared_stand_in{};
    AsyncCompiler* async{};
    std::chrono::steady_clock::time_point queued{};
    u64 SinceQueuedMs() const {
        return u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - queued)
                       .count());
    }
    vk::UniquePipeline stand_in;
    std::atomic<bool> stand_in_ready{false};
    vk::PipelineCache pending_cache;
    std::string pending_name;
    std::atomic<bool> failed{false};
    std::atomic<bool> compile_claimed{false};

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    void BuildDescSetLayout(bool preloading);

private:
    const GeometryReplayStages* geometry{};
    u32 tess_fused{};
    GraphicsPipelineKey key;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
