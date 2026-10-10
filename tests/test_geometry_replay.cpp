// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "vulkan_readback.h"
#include <bit>
#include <filesystem>
#include <fstream>

static void Expect(u32 actual, u32 expected, u32 word) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL replay word %u: %08x != %08x\n", word, actual, expected);
        std::exit(1);
    }
}

int main(int argc, char **argv) {
    using namespace Shader;
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    Backend::SPIRV::GeometryComputeLayout geometry{
        .attribute_dwords = 8, .vertex_stride_dwords = 9, .primitive_stride_dwords = 74};
    bool last_provoking_vertex = true, compile_only = false;
    u32 input_binding = 0;
    bool native_last_provoking = true, remap_negative_depth = false;
    const char *dump_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--first")
            last_provoking_vertex = false;
        else if (std::string_view(argv[i]) == "--compile-only")
            compile_only = true;
        else if (std::string_view(argv[i]) == "--binding-offset")
            input_binding = 7;
        else if (std::string_view(argv[i]) == "--first-hardware")
            native_last_provoking = false;
        else if (std::string_view(argv[i]) == "--negative-one-to-one")
            remap_negative_depth = true;
        else if (!dump_path)
            dump_path = argv[i];
        else
            return 2;
    }
    const auto vertex_code =
        Backend::SPIRV::EmitGeometryReplayVertex(profile, geometry, 8, input_binding + 1,
                                                 last_provoking_vertex, input_binding,
                                                 native_last_provoking, remap_negative_depth);
    if (dump_path) {
        if (std::filesystem::exists(dump_path))
            return 2;
        std::ofstream output(dump_path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(vertex_code.data()), vertex_code.size() * 4);
        if (!output)
            return 2;
    }
    if (compile_only)
        return 0;

    // Gpu provides allocations and a graphics-capable queue. Its empty compute
    // pipeline is never dispatched in this test.
    Info dummy_info{};
    dummy_info.hw_stage = HwStage::Compute;
    dummy_info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::Program program(dummy_info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Backend::Bindings bindings{};
    auto dummy = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    Gpu gpu(dummy, false, false, true);

    VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkRenderPassCreateInfo rpci{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                .subpassCount = 1,
                                .pSubpasses = &subpass};
    VkRenderPass render_pass;
    CHECK(vkCreateRenderPass(gpu.device, &rpci, nullptr, &render_pass));
    VkFramebufferCreateInfo fbci{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                 .renderPass = render_pass,
                                 .width = 1,
                                 .height = 1,
                                 .layers = 1};
    VkFramebuffer framebuffer;
    CHECK(vkCreateFramebuffer(gpu.device, &fbci, nullptr, &framebuffer));
    const VkDescriptorSetLayoutBinding db[] = {
        {input_binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {input_binding + 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dslci{.sType =
                                              VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2,
                                          .pBindings = db};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(gpu.device, &dslci, nullptr, &set_layout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .maxSets = 1,
                                    .poolSizeCount = 1,
                                    .pPoolSizes = &size};
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(gpu.device, &dpci, nullptr, &descriptors));
    VkDescriptorSetAllocateInfo dsai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                     .descriptorPool = descriptors,
                                     .descriptorSetCount = 1,
                                     .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(gpu.device, &dsai, &set));
    VkPipelineLayoutCreateInfo plci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .setLayoutCount = 1,
                                    .pSetLayouts = &set_layout};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(gpu.device, &plci, nullptr, &layout));
    VkShaderModuleCreateInfo smci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = vertex_code.size() * 4,
                                  .pCode = vertex_code.data()};
    VkShaderModule vertex;
    CHECK(vkCreateShaderModule(gpu.device, &smci, nullptr, &vertex));
    VkPipelineShaderStageCreateInfo stage{.sType =
                                              VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                          .module = vertex,
                                          .pName = "main"};
    VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport{0, 0, 1, 1, 0, 1};
    VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vp{.sType =
                                             VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                         .viewportCount = 1,
                                         .pViewports = &viewport,
                                         .scissorCount = 1,
                                         .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .rasterizerDiscardEnable = VK_TRUE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.f};
    VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkGraphicsPipelineCreateInfo gpci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                      .stageCount = 1,
                                      .pStages = &stage,
                                      .pVertexInputState = &vi,
                                      .pInputAssemblyState = &ia,
                                      .pViewportState = &vp,
                                      .pRasterizationState = &raster,
                                      .pMultisampleState = &ms,
                                      .layout = layout,
                                      .renderPass = render_pass};
    VkPipeline graphics;
    CHECK(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &graphics));

    // First instance: 3-vertex strip then 5-vertex strip. The second strip starts
    // at an odd global index, so resetting winding is essential. Also cover a
    // partial strip and input/output descriptor bounds.
    for (u32 test = 0; test < 3; ++test) {
        std::fill_n(gpu.mapped[0], 1024, 0u);
        std::fill_n(gpu.mapped[1], 1024, 0xdeadbeefu);
        for (u32 instance = 0; instance < 3; ++instance) {
            const u32 base = instance * 74;
            gpu.mapped[0][base] = instance == 1 ? 4 : 8;
            for (u32 v = 0; v < 8; ++v) {
                const u32 record = base + 2 + v * 9;
                gpu.mapped[0][record] = instance == 0 && v >= 3 ? 3 : 0;
                for (u32 comp = 0; comp < 8; ++comp)
                    gpu.mapped[0][record + 1 + comp] =
                        std::bit_cast<u32>(float(instance * 100 + v * 10 + comp));
            }
        }
        const VkDescriptorBufferInfo ranges[] = {
            {gpu.buffers[0], 0, (test == 1 ? 2u : 3u) * 74 * 4},
            {gpu.buffers[1], 0, test == 2 ? 18u * 8 * 4 : 4096u}};
        for (u32 b = 0; b < 2; ++b) {
            VkWriteDescriptorSet w{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = set,
                                   .dstBinding = input_binding + b,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   .pBufferInfo = &ranges[b]};
            vkUpdateDescriptorSets(gpu.device, 1, &w, 0, nullptr);
        }
        VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = gpu.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
        VkCommandBuffer cmd;
        CHECK(vkAllocateCommandBuffers(gpu.device, &cai, &cmd));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkRenderPassBeginInfo rpbegin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = render_pass,
                                      .framebuffer = framebuffer,
                                      .renderArea = {{0, 0}, {1, 1}}};
        vkCmdBeginRenderPass(cmd, &rpbegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdDraw(cmd, 18, 3, 0, 0);
        vkCmdEndRenderPass(cmd);
        VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(gpu.queue));
        vkFreeCommandBuffers(gpu.device, gpu.pool, 1, &cmd);
        for (u32 word = 0; word < 1024; ++word) {
            u32 expected = 0xdeadbeef;
            const u32 instance = word / (18 * 8), vindex = (word / 8) % 18, component = word % 8;
            const u32 triangle = vindex / 3;
            const u32 corner = (vindex +
                (last_provoking_vertex && !native_last_provoking ? 2 : 0)) % 3;
            const bool bounded =
                instance < 3 && !(test == 1 && instance >= 2) && !(test == 2 && instance >= 1);
            const bool triangle_valid = instance == 0   ? (triangle == 0 || triangle >= 3)
                                        : instance == 1 ? triangle < 2
                                                        : true;
            if (bounded && triangle_valid) {
                const u32 start = instance == 0 && triangle >= 3 ? 3 : 0;
                const bool odd = (triangle - start) % 2;
                const u32 odd_corner = last_provoking_vertex ? (corner < 2 ? corner ^ 1 : corner)
                                                             : (corner > 0 ? 3 - corner : corner);
                const u32 selected = triangle + (odd ? odd_corner : corner);
                float value = float(instance * 100 + selected * 10 + component);
                if (remap_negative_depth && component == 2) value += 0.5f;
                expected = std::bit_cast<u32>(value);
            }
            Expect(gpu.mapped[1][word], expected, word);
        }
    }
    vkDestroyPipeline(gpu.device, graphics, nullptr);
    vkDestroyShaderModule(gpu.device, vertex, nullptr);
    vkDestroyPipelineLayout(gpu.device, layout, nullptr);
    vkDestroyDescriptorPool(gpu.device, descriptors, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, set_layout, nullptr);
    vkDestroyFramebuffer(gpu.device, framebuffer, nullptr);
    vkDestroyRenderPass(gpu.device, render_pass, nullptr);
    std::puts("PASS: geometry vertex replay, provoking corner/winding after odd-length strips, "
              "incomplete triangles "
              "and bounds");
}
