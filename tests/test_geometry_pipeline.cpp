// SPDX-License-Identifier: GPL-2.0-or-later
// Lowered ES -> GS through production descriptor allocation and deferred recording.
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

static void Expect(u32 actual, u32 expected, const char* what) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: %08x != %08x\n", what, actual, expected);
        std::exit(1);
    }
}

struct Kernel {
    Shader::Info info{};
    Shader::Backend::SPIRV::GeometryComputeLayout layout{};
    vk::UniqueShaderModule module;
    std::unique_ptr<Vulkan::ComputePipeline> pipeline;

    Kernel(bool export_stage, const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
           Vulkan::DescriptorHeap& heap, const Shader::Profile& profile) {
        using namespace Shader;
        info.hw_stage = export_stage ? HwStage::Export : HwStage::Geometry;
        info.sw_stage = export_stage ? SwStage::Vertex : SwStage::Geometry;
        info.buffers.push_back({.used_types = IR::Type::U32});
        RuntimeInfo runtime{};
        runtime.Initialize(info.hw_stage, info.sw_stage);
        if (export_stage) {
            runtime.hw.es.vertex_data_size = 8;
        } else {
            runtime.hw.gs.in_primitive = AmdGpu::PrimitiveType::PointList;
            runtime.hw.gs.num_invocations = 1;
            runtime.hw.gs.output_vertices = 3;
            runtime.hw.gs.in_vertex_data_size = 8;
        }
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        ir.Prologue();
        const IR::Attribute builtins[] = {IR::Attribute::VertexId, IR::Attribute::InstanceId,
                                         IR::Attribute::BaseVertex, IR::Attribute::BaseInstance};
        for (u32 comp = 0; comp < 4; ++comp) {
            info.stores.Set(IR::Attribute::Position0, comp);
            info.stores.Set(IR::Attribute::Param0, comp);
            if (export_stage) {
                info.loads.Set(builtins[comp], 0);
                ir.SetAttribute(IR::Attribute::Position0,
                                ir.BitCast<IR::F32>(ir.GetAttributeU32(builtins[comp])), comp);
                ir.SetAttribute(IR::Attribute::Param0, ir.Imm32(0.f), comp);
            } else {
                ir.SetAttribute(IR::Attribute::Position0,
                                ir.GetAttribute(IR::Attribute::Position0, comp, 0), comp);
                ir.SetAttribute(IR::Attribute::Param0,
                                ir.GetAttribute(IR::Attribute::Param0, comp, 0), comp);
            }
        }
        ir.SetAttribute(IR::Attribute::Param0,
                        ir.BitCast<IR::F32>(IR::U32{
                            ir.LoadBufferU32(1, ir.Imm32(0u), ir.Imm32(0u), {})}),
                        export_stage ? 0 : 3);
        if (!export_stage) {
            ir.EmitVertex();
            ir.EmitVertex();
            ir.EmitVertex();
        }
        IR::Program program(info);
        program.blocks.push_back(&block);
        program.syntax_list.push_back(
            {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
        Backend::Bindings bindings{};
        const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings, &layout);
        Expect(layout.input_binding, 1, "input follows guest buffer");
        Expect(layout.output_binding, 2, "output follows input");
        module = Vulkan::Check(instance.GetDevice().createShaderModuleUnique(
            {.codeSize = code.size() * sizeof(u32), .pCode = code.data()}));
        Vulkan::ComputePipeline::SerializationSupport serial{};
        pipeline = std::make_unique<Vulkan::ComputePipeline>(
            instance, scheduler, heap, profile, vk::PipelineCache{},
            Vulkan::ComputePipelineKey{export_stage ? 1u : 2u}, info, *module, serial, false, 2);
    }
};

int main() {
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    const std::array sizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 192}};
    Vulkan::DescriptorHeap heap(instance, scheduler.GetWorkSemaphore(), sizes, 64);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010600;
    Kernel es(true, instance, scheduler, heap, profile);
    Kernel gs(false, instance, scheduler, heap, profile);
    using VideoCore::Buffer;
    using VideoCore::MemoryType;
    Buffer es_constant(instance, 0, 4, MemoryType::HostUncached);
    Buffer gs_constant(instance, 0, 4, MemoryType::HostUncached);
    Buffer input(instance, 0, 32, MemoryType::HostUncached);
    Buffer exported(instance, 0, (4 + 6 * 8) * 4, MemoryType::DeviceLocal);
    Buffer expanded(instance, 0, 6 * 29 * 4, MemoryType::DeviceLocal);
    Buffer readback(instance, 0, expanded.SizeBytes(), MemoryType::HostCached);
    const u32 first = std::bit_cast<u32>(42.f), second = std::bit_cast<u32>(84.f);
    std::memcpy(es_constant.mapped_data.data(), &first, 4);
    std::memcpy(gs_constant.mapped_data.data(), &second, 4);
    es_constant.Flush(0, 4);
    gs_constant.Flush(0, 4);
    const std::array<u32, 8> header{3, 2, 5, 7, 0, 0, 0, 0};
    std::memcpy(input.mapped_data.data(), header.data(), sizeof(header));
    input.Flush(0, sizeof(header));
    runtime.FillBuffer(&exported, 0, exported.SizeBytes(), 0xdeadbeef);
    runtime.FillBuffer(&expanded, 0, expanded.SizeBytes(), 0xdeadbeef);
    const auto dispatch = [&](Kernel& kernel, Buffer& constants, Buffer& source, Buffer& dest) {
        runtime.FlushBarriers();
        const std::array ranges{vk::DescriptorBufferInfo{constants.Handle(), 0, constants.SizeBytes()},
                                vk::DescriptorBufferInfo{source.Handle(), 0, source.SizeBytes()},
                                vk::DescriptorBufferInfo{dest.Handle(), 0, dest.SizeBytes()}};
        Vulkan::Pipeline::DescriptorWrites writes;
        for (u32 i = 0; i < ranges.size(); ++i) {
            writes.push_back({.dstBinding = i, .descriptorCount = 1,
                              .descriptorType = vk::DescriptorType::eStorageBuffer,
                              .pBufferInfo = &ranges[i]});
        }
        kernel.pipeline->BindResources(writes, Shader::PushData{});
        scheduler.Record([handle = kernel.pipeline->Handle()](vk::CommandBuffer cmd) {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
            cmd.dispatch(1, 1, 1);
        });
        runtime.AccessBuffer(&constants, 0, constants.SizeBytes(), vk::PipelineStageFlagBits2::eComputeShader,
                              vk::AccessFlagBits2::eShaderRead);
        runtime.AccessBuffer(&source, 0, source.SizeBytes(), vk::PipelineStageFlagBits2::eComputeShader,
                              vk::AccessFlagBits2::eShaderRead);
        runtime.AccessBuffer(&dest, 0, dest.SizeBytes(), vk::PipelineStageFlagBits2::eComputeShader,
                              vk::AccessFlagBits2::eShaderWrite);
        scheduler.KickRecording(true);
    };
    dispatch(es, es_constant, input, exported);
    dispatch(gs, gs_constant, exported, expanded);
    const vk::BufferCopy copy{0, 0, expanded.SizeBytes()};
    runtime.CopyBuffer(&expanded, &readback, std::span{&copy, 1});
    runtime.FlushBarriers();
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                                      .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                      .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    });
    scheduler.Finish();
    readback.Invalidate(0, readback.SizeBytes());
    const auto* result = reinterpret_cast<const u32*>(readback.mapped_data.data());
    for (u32 point = 0; point < 6; ++point) {
        Expect(result[point * 29], 3, "emitted count");
        Expect(result[point * 29 + 1], 0, "overflow flag");
        for (u32 vertex = 0; vertex < 3; ++vertex) {
            const u32* row = result + point * 29 + 2 + vertex * 9;
            Expect(row[0], 0, "strip start");
            Expect(row[1], 5 + point % 3, "exported vertex ID");
            Expect(row[2], 7 + point / 3, "exported instance ID");
            Expect(row[3], 5, "base vertex");
            Expect(row[4], 7, "base instance");
            Expect(row[5], first, "ES guest buffer preserved");
            Expect(row[8], second, "GS guest buffer bound independently");
        }
    }
    std::puts("PASS: production ES->GS pipelines, guest + appended descriptors, deferred recording and barriers");
}
