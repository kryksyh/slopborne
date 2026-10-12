// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise production clear/transfer barriers and verify every untouched array face.
#include <array>
#include <cstdio>
#include <cstdlib>
#include "common/slot_vector.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

int main() {
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    for (u32 side : {32u, 64u, 256u}) {
        VideoCore::ImageInfo info;
        info.type = AmdGpu::ImageType::Color2DArray;
        info.pixel_format = vk::Format::eR16G16Sfloat;
        info.size = {side, side, 1};
        info.pitch = side;
        info.num_bits = 32;
        info.resources = {.levels = 1, .layers = 6};
        info.guest_size = side * side * 4 * 6;
        VideoCore::Image image(instance, runtime, views, info);
        VideoCore::Buffer upload(instance, 0, info.guest_size, VideoCore::MemoryType::HostUncached);
        VideoCore::Buffer readback(instance, 0, info.guest_size, VideoCore::MemoryType::HostCached);
        auto* initial = reinterpret_cast<u32*>(upload.mapped_data.data());
        for (u32 face = 0; face < 6; ++face)
            for (u32 pixel = 0; pixel < side * side; ++pixel)
                initial[face * side * side + pixel] = (0x2400u + face * 0x100u) * 0x10001u;
        upload.Flush(0, info.guest_size);
        const vk::BufferImageCopy region{
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 6},
            .imageExtent = {side, side, 1}};
        for (u32 selected = 0; selected < 6; ++selected) {
            runtime.UploadImage(&image, &upload, std::span{&region, 1});
            const float first = selected == 5 ? -0.0f : selected % 2 ? 1.0f : 0.0f;
            const vk::ClearValue clear{
                .color = {.float32 = std::array<float, 4>{first, 1.0f, 0.0f, 0.0f}}};
            runtime.ClearImage(&image, {.base = {.level = 0, .layer = selected},
                                        .extent = {.levels = 1, .layers = 1}}, clear);
            runtime.DownloadImage(&image, &readback, std::span{&region, 1});
            runtime.FlushBarriers();
            scheduler.Record([](vk::CommandBuffer cmd) {
                const vk::MemoryBarrier2 host{
                    .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                    .dstAccessMask = vk::AccessFlagBits2::eHostRead};
                cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
            });
            scheduler.Finish();
            readback.Invalidate(0, info.guest_size);
            const auto* actual = reinterpret_cast<const u32*>(readback.mapped_data.data());
            const u32 cleared = 0x3c000000u | (selected == 5 ? 0x8000u : selected % 2 ? 0x3c00u : 0u);
            for (u32 face = 0; face < 6; ++face)
                for (u32 pixel = 0; pixel < side * side; ++pixel) {
                    const u32 index = face * side * side + pixel;
                    const u32 expected = face == selected ? cleared : initial[index];
                    if (actual[index] != expected) {
                        std::fprintf(stderr, "FAIL side=%u selected=%u face=%u pixel=%u: %08x != %08x\n",
                                     side, selected, face, pixel, actual[index], expected);
                        return 1;
                    }
                }
        }
    }
    std::puts("PASS: clear all six slice positions at 32/64/256; untouched faces and signed zero preserved");
}
