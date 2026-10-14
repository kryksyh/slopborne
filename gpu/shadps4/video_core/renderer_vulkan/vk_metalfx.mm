// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Apple MetalFX temporal upscaling (see vk_metalfx.h).

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_metalfx.h"

#include <vulkan/vulkan_metal.h>

namespace Vulkan {

// Auto exposure off: its network compile (MPSGraph) threw a filesystem error inside Apple's
// framework when the HDR scaler was made, ending the game (native 1080p, 2026-10-14).
// BB_METALFX_AUTO_EXPOSURE=1 turns it back on.
static bool AutoExposure() {
    static const bool on = [] {
        const char* env = std::getenv("BB_METALFX_AUTO_EXPOSURE");
        return env && env[0] == '1';
    }();
    return on;
}

struct MetalFx::Impl {
    vk::Device device;
    PFN_vkExportMetalObjectsEXT export_objects{};
    id<MTLDevice> mtl_device;
    id<MTLCommandQueue> queue;
    id<MTLSharedEvent> event;
    vk::UniqueSemaphore semaphore;
    u64 value{};

    id<MTLFXTemporalScaler> scaler;
    MTLFXTemporalScalerDescriptor* key;
    id<MTLFXSpatialScaler> spatial;
    MTLFXSpatialScalerDescriptor* spatial_key;
    id<MTLTexture> zero_mask;
    bool usage_checked{};

    /// An all-zero R8 mask of the input size, for frames without a reactive mask.
    id<MTLTexture> ZeroMask(NSUInteger width, NSUInteger height) {
        if (zero_mask && zero_mask.width == width && zero_mask.height == height) {
            return zero_mask;
        }
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        zero_mask = [mtl_device newTextureWithDescriptor:desc];
        std::vector<u8> zeros(width * height);
        [zero_mask replaceRegion:MTLRegionMake2D(0, 0, width, height)
                     mipmapLevel:0
                       withBytes:zeros.data()
                     bytesPerRow:width];
        return zero_mask;
    }

    id<MTLTexture> Texture(vk::Image image) const {
        VkExportMetalTextureInfoEXT texture{.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
                                            .image = image,
                                            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT};
        VkExportMetalObjectsInfoEXT info{.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                                         .pNext = &texture};
        export_objects(device, &info);
        return texture.mtlTexture;
    }

    /// Makes the scaler for these formats and sizes (MTLPixelFormatInvalid: no reactive mask).
    bool MakeScaler(MTLPixelFormat color, MTLPixelFormat depth, MTLPixelFormat motion,
                    MTLPixelFormat output, NSUInteger width, NSUInteger height,
                    NSUInteger out_width, NSUInteger out_height, bool hdr, MTLPixelFormat reactive) {
        if (scaler && key.colorTextureFormat == color && key.depthTextureFormat == depth &&
            key.motionTextureFormat == motion && key.outputTextureFormat == output &&
            key.inputWidth == width && key.inputHeight == height &&
            key.outputWidth == out_width && key.outputHeight == out_height &&
            key.autoExposureEnabled == (hdr && AutoExposure()) &&
            key.reactiveMaskTextureEnabled == (reactive != MTLPixelFormatInvalid)) {
            return true;
        }
        MTLFXTemporalScalerDescriptor* desc = [MTLFXTemporalScalerDescriptor new];
        desc.colorTextureFormat = color;
        desc.depthTextureFormat = depth;
        desc.motionTextureFormat = motion;
        desc.outputTextureFormat = output;
        desc.inputWidth = width;
        desc.inputHeight = height;
        desc.outputWidth = out_width;
        desc.outputHeight = out_height;
        desc.autoExposureEnabled = hdr && AutoExposure();
        // The scene may fill only part of the color image (the patched render size).
        desc.inputContentPropertiesEnabled = YES;
        desc.inputContentMinScale = 1.0f;
        desc.inputContentMaxScale = 3.0f;
        if (reactive != MTLPixelFormatInvalid) {
            desc.reactiveMaskTextureEnabled = YES;
            desc.reactiveMaskTextureFormat = reactive;
        }
        scaler = [desc newTemporalScalerWithDevice:mtl_device];
        key = desc;
        usage_checked = false;
        if (!scaler) {
            std::printf("MetalFX: scaler creation failed (%lux%lu -> %lux%lu)\n",
                        (unsigned long)width, (unsigned long)height, (unsigned long)out_width,
                        (unsigned long)out_height);
            return false;
        }
        std::printf("MetalFX: temporal scaler %lux%lu -> %lux%lu (formats %lu/%lu/%lu/%lu)%s%s\n",
                    (unsigned long)width, (unsigned long)height, (unsigned long)out_width,
                    (unsigned long)out_height, (unsigned long)color, (unsigned long)depth,
                    (unsigned long)motion, (unsigned long)output,
                    hdr ? (AutoExposure() ? " (HDR, auto exposure)" : " (HDR)") : "",
                    reactive != MTLPixelFormatInvalid ? " (reactive mask)" : "");
        return true;
    }

    bool EnsureScaler(id<MTLTexture> color, id<MTLTexture> depth, id<MTLTexture> motion,
                      id<MTLTexture> output, id<MTLTexture> reactive, bool hdr) {
        if (!MakeScaler(color.pixelFormat, depth.pixelFormat, motion.pixelFormat,
                        output.pixelFormat, color.width, color.height, output.width,
                        output.height, hdr, reactive ? reactive.pixelFormat : MTLPixelFormatInvalid)) {
            return false;
        }
        if (usage_checked) {
            return true;
        }
        const auto missing = [](id<MTLTexture> texture, MTLTextureUsage needed) {
            return (texture.usage & needed) != needed;
        };
        if (missing(color, scaler.colorTextureUsage) || missing(depth, scaler.depthTextureUsage) ||
            missing(motion, scaler.motionTextureUsage) ||
            missing(output, scaler.outputTextureUsage) ||
            (reactive && missing(reactive, scaler.reactiveTextureUsage))) {
            std::printf("MetalFX: texture usage missing (color %lu/%lu depth %lu/%lu motion "
                        "%lu/%lu output %lu/%lu)\n",
                        (unsigned long)color.usage, (unsigned long)scaler.colorTextureUsage,
                        (unsigned long)depth.usage, (unsigned long)scaler.depthTextureUsage,
                        (unsigned long)motion.usage, (unsigned long)scaler.motionTextureUsage,
                        (unsigned long)output.usage, (unsigned long)scaler.outputTextureUsage);
            scaler = nil;
            return false;
        }
        usage_checked = true;
        return true;
    }
};

MetalFx::MetalFx() : impl{std::make_unique<Impl>()} {}
MetalFx::~MetalFx() = default;

std::unique_ptr<MetalFx> MetalFx::Create(const Instance& instance) {
    if (!instance.IsMetalObjectsSupported()) {
        return nullptr;
    }
    @autoreleasepool {
        std::unique_ptr<MetalFx> metalfx{new MetalFx()};
        Impl& impl = *metalfx->impl;
        impl.device = instance.GetDevice();
        impl.export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
            impl.device.getProcAddr("vkExportMetalObjectsEXT"));
        if (!impl.export_objects) {
            return nullptr;
        }
        VkExportMetalDeviceInfoEXT device_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
        VkExportMetalObjectsInfoEXT info{.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                                         .pNext = &device_info};
        impl.export_objects(impl.device, &info);
        impl.mtl_device = device_info.mtlDevice;
        if (!impl.mtl_device || ![MTLFXTemporalScalerDescriptor supportsDevice:impl.mtl_device]) {
            return nullptr;
        }
        impl.queue = [impl.mtl_device newCommandQueue];
        impl.event = [impl.mtl_device newSharedEvent];

        VkImportMetalSharedEventInfoEXT import{
            .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT,
            .mtlSharedEvent = impl.event};
        const vk::SemaphoreTypeCreateInfo type{.pNext = &import,
                                               .semaphoreType = vk::SemaphoreType::eTimeline};
        auto [result, semaphore] = impl.device.createSemaphoreUnique({.pNext = &type});
        if (result != vk::Result::eSuccess) {
            return nullptr;
        }
        impl.semaphore = std::move(semaphore);
        // The semaphore must be that event: Vulkan waits on what MetalFX signals.
        VkExportMetalSharedEventInfoEXT event_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
            .semaphore = *impl.semaphore};
        info.pNext = &event_info;
        impl.export_objects(impl.device, &info);
        if (event_info.mtlSharedEvent != impl.event) {
            std::printf("MetalFX: unavailable, the Vulkan semaphore is not a Metal shared event\n");
            return nullptr;
        }
        std::printf("MetalFX: available on %s\n", impl.mtl_device.name.UTF8String);
        return metalfx;
    }
}

void MetalFx::Prewarm(u32 width, u32 height, u32 out_width, u32 out_height, bool reactive) {
    @autoreleasepool {
        // The formats a patched session's upscale uses (UI path: the game's 8-bit frame, the
        // D32S8 depth buffer, the R16G16 motion image and the RGBA16F output image).
        impl->MakeScaler(MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float_Stencil8,
                         MTLPixelFormatRG16Float, MTLPixelFormatRGBA16Float, width, height,
                         out_width, out_height, false,
                         reactive ? MTLPixelFormatR8Unorm : MTLPixelFormatInvalid);
    }
}

vk::Semaphore MetalFx::Semaphore() const {
    return *impl->semaphore;
}

u64 MetalFx::NextReady() {
    impl->value += 2;
    return impl->value - 1;
}

bool MetalFx::Encode(const Frame& frame, u64 ready) {
    @autoreleasepool {
        id<MTLTexture> color = impl->Texture(frame.color);
        id<MTLTexture> depth = impl->Texture(frame.depth);
        id<MTLTexture> motion = impl->Texture(frame.motion);
        id<MTLTexture> output = impl->Texture(frame.output);
        if (!color || !depth || !motion || !output) {
            std::printf("MetalFX: a Vulkan image has no Metal texture\n");
            return false;
        }
        // A scaler is made per configuration: switching the mask per frame would rebuild it.
        id<MTLTexture> reactive = !frame.reactive_enabled ? nil
            : frame.reactive ? impl->Texture(frame.reactive)
                             : impl->ZeroMask(color.width, color.height);
        if (!impl->EnsureScaler(color, depth, motion, output, reactive, frame.hdr)) {
            return false;
        }
        id<MTLFXTemporalScaler> scaler = impl->scaler;
        scaler.colorTexture = color;
        scaler.depthTexture = depth;
        scaler.motionTexture = motion;
        scaler.outputTexture = output;
        scaler.reactiveMaskTexture = reactive;
        scaler.inputContentWidth = frame.width;
        scaler.inputContentHeight = frame.height;
        scaler.jitterOffsetX = frame.jitter_x;
        scaler.jitterOffsetY = frame.jitter_y;
        // Render pixels, current to previous: what FSR 3 gets too.
        scaler.motionVectorScaleX = 1.0f;
        scaler.motionVectorScaleY = 1.0f;
        scaler.depthReversed = NO;
        scaler.reset = frame.reset;
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        [command encodeWaitForEvent:impl->event value:ready];
        [scaler encodeToCommandBuffer:command];
        [command encodeSignalEvent:impl->event value:ready + 1];
        [command commit];
        return true;
    }
}

bool MetalFx::EncodeSpatial(vk::Image color_image, u32 width, u32 height, vk::Image output_image,
                            u32 out_width, u32 out_height, u64 ready) {
    @autoreleasepool {
        id<MTLTexture> color = impl->Texture(color_image);
        id<MTLTexture> output = impl->Texture(output_image);
        if (!color || !output) {
            std::printf("MetalFX spatial: a Vulkan image has no Metal texture\n");
            return false;
        }
        MTLFXSpatialScalerDescriptor* key = impl->spatial_key;
        if (!impl->spatial || key.colorTextureFormat != color.pixelFormat ||
            key.outputTextureFormat != output.pixelFormat || key.inputWidth != color.width ||
            key.inputHeight != color.height || key.outputWidth != out_width ||
            key.outputHeight != out_height) {
            MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
            desc.colorTextureFormat = color.pixelFormat;
            desc.outputTextureFormat = output.pixelFormat;
            desc.inputWidth = color.width;
            desc.inputHeight = color.height;
            desc.outputWidth = out_width;
            desc.outputHeight = out_height;
            // The game's final 8-bit frame is display-encoded.
            desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            impl->spatial = [desc newSpatialScalerWithDevice:impl->mtl_device];
            impl->spatial_key = desc;
            if (!impl->spatial) {
                std::printf("MetalFX spatial: scaler creation failed\n");
                return false;
            }
            const auto missing = [](id<MTLTexture> texture, MTLTextureUsage needed) {
                return (texture.usage & needed) != needed;
            };
            if (missing(color, impl->spatial.colorTextureUsage) ||
                missing(output, impl->spatial.outputTextureUsage)) {
                std::printf("MetalFX spatial: texture usage missing (color %lu/%lu output %lu/%lu)\n",
                            (unsigned long)color.usage, (unsigned long)impl->spatial.colorTextureUsage,
                            (unsigned long)output.usage,
                            (unsigned long)impl->spatial.outputTextureUsage);
                impl->spatial = nil;
                return false;
            }
            std::printf("MetalFX: spatial scaler %ux%u -> %ux%u\n", width, height, out_width,
                        out_height);
        }
        id<MTLFXSpatialScaler> scaler = impl->spatial;
        scaler.colorTexture = color;
        scaler.outputTexture = output;
        scaler.inputContentWidth = width;
        scaler.inputContentHeight = height;
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        [command encodeWaitForEvent:impl->event value:ready];
        [scaler encodeToCommandBuffer:command];
        [command encodeSignalEvent:impl->event value:ready + 1];
        [command commit];
        return true;
    }
}

} // namespace Vulkan
