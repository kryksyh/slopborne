// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Shared Vulkan readback harness for shader-backend regression tests.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>
#include "shader_recompiler/info.h"
#include "../src/vulkan_portability.h"

#define CHECK(call)                                                                                \
    do {                                                                                           \
        auto check_result = (call);                                                                \
        if (check_result != VK_SUCCESS) {                                                          \
            std::fprintf(stderr, "%s: Vulkan error %d\n", #call, int(check_result));               \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

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
    Gpu(const std::vector<u32> &code, bool images, bool dma = false, bool vertex_stores = false)
        : buffer_count(dma ? 4 : 3) {
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
        features.vertexPipelineStoresAndAtomics = vertex_stores;
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
    void run(bool present, u32 offset = 0, const u32* values = nullptr,
             std::span<const u32> custom_input = {}, u32 output_bytes = 4096) {
        for (u32 i = 0; i < buffer_count; ++i)
            std::memset(mapped[i], 0, 4096);
        const u32 data[] = {11, 22, 33, 44};
        if (buffer_count == 3)
            std::memcpy(mapped[0], values ? values : data, sizeof(data));
        for (int i = 0; i < buffer_count; ++i)
            mapped[buffer_count - 1][i] = 1;
        mapped[buffer_count - 1][0] = present;
        mapped[buffer_count - 1][Shader::NUM_BUFFERS] = present;
        if (!custom_input.empty()) {
            if (custom_input.size_bytes() > 4096 || output_bytes == 0 || output_bytes > 4096)
                std::exit(1);
            std::memcpy(mapped[0], custom_input.data(), custom_input.size_bytes());
            std::fill_n(mapped[1], 1024, 0xdeadbeefu);
            const VkDescriptorBufferInfo ranges[] = {
                {buffers[0], 0, custom_input.size_bytes()}, {buffers[1], 0, output_bytes}};
            for (u32 binding = 0; binding < 2; ++binding) {
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = set, .dstBinding = binding, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ranges[binding]};
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
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
