// SPDX-License-Identifier: GPL-2.0-or-later
// Production shader backend: GPU readback with hardware robustness disabled.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "../src/vulkan_portability.h"
#define CHECK(call)                                                                                \
    do {                                                                                           \
        auto check_result = (call);                                                                \
        if (check_result != VK_SUCCESS) {                                                          \
            std::fprintf(stderr, "%s: Vulkan error %d\n", #call, int(check_result));               \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

static std::vector<u32> shader(bool images, bool widths = false, bool shared = false, bool dma = false,
                               bool readonly = false) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    for (int i = 0; i < 3; ++i) {
        info.buffers.push_back(
            {.used_types = IR::Type::U32,
             .buffer_type = i == 2 ? BufferType::ResourcePresence : BufferType::Guest,
             .is_written = i != 2});
    }
    if (dma) {
        info.buffers[0].buffer_type = BufferType::BdaPagetable;
        info.buffers[0].used_types = IR::Type::U64;
        info.buffers[2].buffer_type = BufferType::FaultBuffer;
        info.buffers[2].is_written = true;
        info.buffers.push_back({.used_types = IR::Type::U32,
                                .buffer_type = BufferType::ResourcePresence});
        info.uses_dma = true;
        info.readconst_types = Info::ReadConstType::Dynamic;
    }
    if (widths)
        info.buffers[0].used_types |= IR::Type::U8 | IR::Type::U16 | IR::Type::U64 | IR::Type::F32;
    if (readonly)
        info.buffers[0].is_written = false;
    if (images) {
        info.images.push_back({.is_written = true});
        info.has_storage_images = true;
    }
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    if (shared) {
        runtime.hw.cs.workgroup_size = {64, 1, 1};
        runtime.hw.cs.shared_memory_size = 256;
        info.shared_types = IR::Type::U32;
        info.loads.Set(IR::Attribute::LocalInvocationIndex, 0);
    }
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    const auto src = ir.Imm32(0u), dst = ir.Imm32(1u);
    auto save = [&](u32 slot, const IR::Value &value) {
        ir.StoreBufferU32(1, dst, ir.Imm32(slot), value, {});
    };
    if (dma) {
        const u64 addresses[] = {0, 65536, 0x1000000000000ULL, 0xffffffffffffffffULL};
        for (u32 i = 0; i < 4; ++i) {
            const auto address = ir.CompositeConstruct(ir.Imm32(u32(addresses[i])),
                                                      ir.Imm32(u32(addresses[i] >> 32)));
            save(i, ir.ReadConst(address, ir.Imm32(0u)));
        }
    } else if (shared) {
        const auto lane = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const auto peer = ir.BitwiseXor(lane, ir.Imm32(32u));
        ir.WriteShared(32, ir.IAdd(lane, ir.Imm32(1u)), ir.ShiftLeftLogical(lane, ir.Imm32(2u)));
        const auto value = ir.LoadShared(32, false, ir.ShiftLeftLogical(peer, ir.Imm32(2u)));
        ir.StoreBufferU32(1, dst, lane, value, {});
    } else if (widths) {
        save(0, ir.UConvert(32, ir.LoadBufferU8(src, ir.Imm32(8u), {})));
        save(1, ir.UConvert(32, ir.LoadBufferU8(src, ir.Imm32(12u), {})));
        save(2, ir.UConvert(32, ir.LoadBufferU16(src, ir.Imm32(4u), {})));
        save(3, ir.UConvert(32, ir.LoadBufferU16(src, ir.Imm32(6u), {})));
        save(4, ir.CompositeExtract(ir.UnpackUint2x32(ir.LoadBufferU64(src, ir.Imm32(0u), {})), 0));
        save(5, ir.CompositeExtract(ir.UnpackUint2x32(ir.LoadBufferU64(src, ir.Imm32(1u), {})), 0));
        save(6, ir.BitCast<IR::U32>(IR::F32{ir.LoadBufferF32(1, src, ir.Imm32(2u), {})}));
        save(7, ir.BitCast<IR::U32>(IR::F32{ir.LoadBufferF32(1, src, ir.Imm32(3u), {})}));
        ir.StoreBufferU8(src, ir.Imm32(12u), ir.Imm8(255), {});
        ir.StoreBufferU16(src, ir.Imm32(6u), ir.Imm16(65535), {});
        ir.StoreBufferU64(src, ir.Imm32(1u), ir.Imm64(0xffffffffffffffffULL), {});
        ir.StoreBufferF32(1, src, ir.Imm32(3u), ir.Imm32(123.0f), {});
    } else if (readonly) {
        save(0, ir.LoadBufferU32(1, src, ir.Imm32(2u), {}));
        save(1, ir.ReadConstBuffer(src, ir.Imm32(3u), {}));
        ir.StoreBufferU32(4, dst, ir.Imm32(2u),
                         ir.LoadBufferU32(4, src, ir.Imm32(2u), {}), {});
        save(6, ir.LoadBufferU32(1, src, ir.Imm32(0xffffffffu), {}));
        save(7, ir.ReadConstBuffer(src, ir.Imm32(0u), {}));
    } else if (!images) {
        save(0, ir.LoadBufferU32(1, src, ir.Imm32(2u), {}));
        save(1, ir.LoadBufferU32(1, src, ir.Imm32(3u), {}));
        save(2, ir.ReadConstBuffer(src, ir.Imm32(3u), {}));
        save(3, ir.BufferAtomicIAdd(src, ir.Imm32(0u), ir.Imm32(5u), {}));
        save(4, ir.BufferAtomicIAdd(src, ir.Imm32(3u), ir.Imm32(5u), {}));
        const auto vector = ir.LoadBufferU32(4, src, ir.Imm32(2u), {});
        ir.StoreBufferU32(4, dst, ir.Imm32(5u), vector, {});
        save(9, ir.LoadBufferU32(1, src, ir.Imm32(0xffffffffu), {}));
        ir.StoreBufferU32(
            4, src, ir.Imm32(2u),
            ir.CompositeConstruct(ir.Imm32(7u), ir.Imm32(8u), ir.Imm32(9u), ir.Imm32(10u)), {});
        ir.StoreBufferU32(1, src, ir.Imm32(0xffffffffu), ir.Imm32(99u), {});
        save(10, ir.LoadBufferU32(1, src, ir.Imm32(0u), {}));
    } else {
        const auto xy = ir.CompositeConstruct(ir.Imm32(0u), ir.Imm32(0u));
        const auto color =
            ir.CompositeConstruct(ir.Imm32(1.0f), ir.Imm32(2.0f), ir.Imm32(3.0f), ir.Imm32(4.0f));
        ir.ImageWrite(src, xy, {}, {}, color, {});
        const auto read = ir.ImageRead(src, xy, {}, {}, {});
        for (u32 i = 0; i < 4; ++i)
            save(i, ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(read, i)}));
        const auto dims = ir.ImageQueryDimension(src, ir.Imm32(0u), ir.Imm1(false), {});
        ir.StoreBufferU32(4, dst, ir.Imm32(4u), dims, {});
    }
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.emulate_robustness = true;
    profile.bounded_buffer_reads = true;
    profile.sparse_page_shift = 16;
    profile.needs_buffer_offsets = true;
    profile.support_int8 = profile.support_int16 = profile.support_int64 = true;
    if (shared) {
        profile.subgroup_size = 32;
        profile.needs_lds_barriers = true;
        Optimization::SharedMemoryBarrierPass(program, runtime, profile);
    }
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

struct Gpu {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    VkBuffer buffers[4]{};
    VkDeviceMemory allocations[4]{};
    u32 *mapped[4]{};
    VkImage image{};
    VkDeviceMemory image_memory{};
    VkImageView view{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    u32 buffer_count;
    Gpu(const std::vector<u32> &code, bool images, bool dma = false) : buffer_count(dma ? 4 : 3) {
        VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app};
        bb_vulkan_portability_instance(&ci);
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        u32 count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count)
            std::exit(1);
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        physical = devices[0];
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
            ++family;
        if (family == count)
            std::exit(1);
        const float priority = 1;
        VkDeviceQueueCreateInfo qci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = family,
                                    .queueCount = 1,
                                    .pQueuePriorities = &priority};
        VkPhysicalDeviceVulkan12Features f12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .storageBuffer8BitAccess = VK_TRUE,
            .shaderInt8 = VK_TRUE,
            .bufferDeviceAddress = VK_TRUE};
        VkPhysicalDeviceVulkan11Features f11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = &f12,
            .storageBuffer16BitAccess = VK_TRUE};
        VkPhysicalDeviceFeatures features{.shaderStorageImageExtendedFormats = VK_TRUE,
                                          .shaderStorageImageReadWithoutFormat = VK_TRUE,
                                          .shaderStorageImageWriteWithoutFormat = VK_TRUE,
                                          .shaderInt64 = VK_TRUE,
                                          .shaderInt16 = VK_TRUE};
        VkDeviceCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .pNext = &f11,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci,
                               .pEnabledFeatures = &features};
        bb_vulkan_portability_device(physical, &dci);
        CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        std::printf("Robustness test GPU: %s (hardware robustness disabled)\n", props.deviceName);
        for (int i = 0; i < buffer_count; ++i) {
            VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = 4096,
                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            CHECK(vkCreateBuffer(device, &bi, nullptr, &buffers[i]));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, buffers[i], &req);
            allocations[i] = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            CHECK(vkBindBufferMemory(device, buffers[i], allocations[i], 0));
            CHECK(vkMapMemory(device, allocations[i], 0, 4096, 0,
                              reinterpret_cast<void **>(&mapped[i])));
        }
        if (images) {
            VkImageCreateInfo ii{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                 .extent = {1, 1, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            CHECK(vkCreateImage(device, &ii, nullptr, &image));
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device, image, &req);
            image_memory = allocate(req, 0);
            CHECK(vkBindImageMemory(device, image, image_memory, 0));
            VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = ii.format,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            CHECK(vkCreateImageView(device, &vi, nullptr, &view));
        }
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (u32 i = 0; i < buffer_count; ++i)
            bindings.push_back(
                {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        if (images)
            bindings.push_back(
                {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo sl{.sType =
                                               VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = u32(bindings.size()),
                                           .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &sl, nullptr, &set_layout));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_count},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1,
                                       .poolSizeCount = 2,
                                       .pPoolSizes = sizes};
        CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
        VkDescriptorSetAllocateInfo da{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = descriptors,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &set_layout};
        CHECK(vkAllocateDescriptorSets(device, &da, &set));
        VkDescriptorBufferInfo infos[] = {
            {buffers[0], 0, 12}, {buffers[1], 0, 4096}, {buffers[2], 0, 4096}, {buffers[3], 0, 4096}};
        for (u32 i = 0; i < buffer_count; ++i) {
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &infos[i]};
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
        if (images) {
            VkDescriptorImageInfo ii{.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 3,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii};
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Shader::PushData)};
        VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &set_layout,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &push};
        CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
        VkShaderModuleCreateInfo sm{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = code.size() * 4,
                                    .pCode = code.data()};
        VkShaderModule module;
        CHECK(vkCreateShaderModule(device, &sm, nullptr, &module));
        VkComputePipelineCreateInfo pi{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module,
                      .pName = "main"},
            .layout = layout};
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline));
        vkDestroyShaderModule(device, module, nullptr);
        VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    }
    VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        u32 i = 0;
        while (i < mem.memoryTypeCount && (!(req.memoryTypeBits & (1u << i)) ||
                                           (mem.memoryTypes[i].propertyFlags & flags) != flags))
            ++i;
        if (i == mem.memoryTypeCount)
            std::exit(1);
        VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = i};
        VkDeviceMemory result;
        CHECK(vkAllocateMemory(device, &ai, nullptr, &result));
        return result;
    }
    void run(bool present, u32 offset = 0) {
        for (u32 i = 0; i < buffer_count; ++i)
            std::memset(mapped[i], 0, 4096);
        const u32 data[] = {11, 22, 33, 44};
        if (buffer_count == 3)
            std::memcpy(mapped[0], data, sizeof(data));
        for (int i = 0; i < buffer_count; ++i)
            mapped[buffer_count - 1][i] = 1;
        mapped[buffer_count - 1][0] = present;
        mapped[buffer_count - 1][Shader::NUM_BUFFERS] = present;
        VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
        VkCommandBuffer cmd;
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        if (image) {
            VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkClearColorValue clear{.float32 = {9, 9, 9, 9}};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                 &b.subresourceRange);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &b);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        Shader::PushData push{};
        push.buf_offsets[0] = offset;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        if (image) {
            VkMemoryBarrier before{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask =
                                       VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            VkBufferImageCopy region{.bufferOffset = 512,
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {1, 1, 1}};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, buffers[1], 1, &region);
        }
        VkMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask =
                              VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
    ~Gpu() {
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, descriptors, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (image_memory)
            vkFreeMemory(device, image_memory, nullptr);
        for (int i = 0; i < buffer_count; ++i) {
            vkUnmapMemory(device, allocations[i]);
            vkDestroyBuffer(device, buffers[i], nullptr);
            vkFreeMemory(device, allocations[i], nullptr);
        }
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
static void expect(u32 actual, u32 expected, const char *label, u32 index) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s[%u]: %u != %u\n", label, index, actual, expected);
        std::exit(1);
    }
}
int main(int argc, char **argv) {
    setenv("BB_DIRECT_MEMORY_ACCESS", "1", 1);
    {
        Shader::Info info{};
        std::array<u32, 16> user_data{};
        info.user_data = user_data;
        Shader::Gcn::VertexAttribute attr{};
        attr.dword_offset = 8;
        attr.inst_offset = 16;
        const auto missing = attr.GetSharp(info);
        if (missing) {
            std::fprintf(stderr, "Null vertex table became a bound descriptor\n");
            return 1;
        }
        std::array<AmdGpu::Buffer, 3> table{};
        table[2].base_address = 0x123400;
        table[2].num_records = 4;
        const auto pointer = table.data();
        std::memcpy(user_data.data(), &pointer, sizeof(pointer));
        if (attr.GetSharp(info).base_address != 0x123410) {
            std::fprintf(stderr, "Valid vertex descriptor offset was not preserved\n");
            return 1;
        }
        std::puts("PASS: null vertex tables and valid descriptor offsets");
    }
    const auto dir = argc > 1 ? argv[1] : "out/macos/robustness";
    std::filesystem::create_directories(dir);
    for (int mode = 0; mode < 6; ++mode) {
        const bool images = mode == 1, widths = mode == 2, shared = mode == 3, dma = mode == 4;
        const bool readonly = mode == 5;
        const auto code = shader(images, widths, shared, dma, readonly);
        std::ofstream out(std::filesystem::path(dir) / (images   ? "images.spv"
                                                        : readonly ? "readonly.spv"
                                                        : dma ? "dma.spv"
                                                        : shared ? "shared.spv"
                                                        : widths ? "widths.spv"
                                                                 : "buffers.spv"),
                          std::ios::binary);
        out.write(reinterpret_cast<const char *>(code.data()), code.size() * 4);
        out.close();
        if (argc > 2 && std::strcmp(argv[2], "--emit-only") == 0)
            continue;
        Gpu gpu(code, images, dma);
        if (dma) {
            gpu.run(true);
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[1][i], 0, "DMA missing/invalid page", i);
            expect(gpu.mapped[2][0], 1, "DMA valid missing page fault", 0);
            for (u32 i = 1; i < 1024; ++i)
                expect(gpu.mapped[2][i], 0, "DMA fault bounds", i);
            std::puts("PASS: DMA page bounds, 64-bit page overflow and missing-page faults");
            continue;
        }
        if (shared) {
            for (int repeat = 0; repeat < 16; ++repeat) {
                gpu.run(true);
                for (u32 lane = 0; lane < 64; ++lane)
                    expect(gpu.mapped[1][lane], (lane ^ 32u) + 1, "wave64 shared exchange", lane);
            }
            std::puts("PASS: 64-lane shared memory exchange across 32-lane subgroups");
            continue;
        }
        gpu.run(false);
        for (u32 i = 0; i < 11; ++i)
            expect(gpu.mapped[1][i], 0, "null result", i);
        for (u32 i = 0; i < 4; ++i)
            expect(gpu.mapped[0][i], 11 * (i + 1), "null backing", i);
        if (images) {
            const float untouched = 9;
            u32 bits;
            std::memcpy(&bits, &untouched, 4);
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[1][128 + i], bits, "null image write isolation", i);
        }
        gpu.run(true);
        if (readonly) {
            const u32 expected[] = {33, 0, 33, 0, 0, 0, 0, 11};
            for (u32 i = 0; i < 8; ++i)
                expect(gpu.mapped[1][i], expected[i], "readonly bounds result", i);
            gpu.run(true, 4);
            expect(gpu.mapped[1][6], 0, "readonly address overflow", 6);
            expect(gpu.mapped[1][7], 22, "readonly shifted constant buffer", 7);
            std::puts("PASS: readonly redirected bounds, partial vectors, null bindings and overflow");
            continue;
        } else if (widths) {
            const u32 expected[] = {33, 0, 33, 0, 11, 0, 33, 0};
            for (u32 i = 0; i < 8; ++i)
                expect(gpu.mapped[1][i], expected[i], "typed buffer result", i);
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[0][i], 11 * (i + 1), "typed discarded writes", i);
        } else if (!images) {
            const u32 expected[] = {33, 0, 0, 11, 0, 33, 0, 0, 0, 0, 16};
            for (u32 i = 0; i < 11; ++i)
                expect(gpu.mapped[1][i], expected[i], "buffer result", i);
            const u32 backing[] = {16, 22, 7, 44};
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[0][i], backing[i], "buffer backing", i);
            gpu.run(true, 4);
            expect(gpu.mapped[1][9], 0, "address overflow read", 0);
            expect(gpu.mapped[0][0], 11, "address overflow write", 0);
        } else {
            const float expected[] = {1, 2, 3, 4};
            for (u32 i = 0; i < 4; ++i) {
                u32 bits;
                std::memcpy(&bits, &expected[i], 4);
                expect(gpu.mapped[1][i], bits, "image result", i);
            }
        }
        std::printf(
            "PASS: %s\n",
            images ? "null image reads/writes/queries and valid image access"
            : widths
                ? "8/16/64-bit and float buffer bounds and null access"
                : "buffer bounds, partial vectors, atomics, null bindings and address overflow");
    }
}
