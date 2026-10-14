// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: Apple MetalFX temporal upscaling (macOS). The Vulkan images are MoltenVK's Metal
// textures (VK_EXT_metal_objects). MetalFX encodes into its own Metal command buffer between
// two Vulkan submissions, ordered by a Metal shared event that is also a Vulkan timeline
// semaphore.

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

class MetalFx {
public:
    /// Null without VK_EXT_metal_objects or a MetalFX-capable GPU.
    static std::unique_ptr<MetalFx> Create(const Instance& instance);
    ~MetalFx();

    struct Frame {
        vk::Image color, depth, motion, output;
        vk::Image reactive; ///< R8 reactive mask this frame, or null (then none is reactive)
        bool reactive_enabled; ///< the scaler takes a reactive mask (stable across frames)
        u32 width, height; ///< scene content in the color/depth images
        float jitter_x, jitter_y;
        bool reset;
        bool hdr;
    };

    /// Makes a patched session's scaler before the game loads: its network compile then does
    /// not run during loading.
    void Prewarm(u32 width, u32 height, u32 out_width, u32 out_height, bool reactive);
    [[nodiscard]] vk::Semaphore Semaphore() const;
    /// The value the Vulkan submission before the upscale signals; the upscale signals the next.
    u64 NextReady();
    /// Encodes the upscale to run after Semaphore() reaches `ready`; it signals `ready + 1`.
    bool Encode(const Frame& frame, u64 ready);
    /// MetalFX spatial upscale (one frame, no history) of the color image's width x height
    /// content into the output's out_width x out_height; ordered like Encode.
    bool EncodeSpatial(vk::Image color, u32 width, u32 height, vk::Image output, u32 out_width,
                       u32 out_height, u64 ready);

private:
    MetalFx();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
