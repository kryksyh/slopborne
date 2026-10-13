// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MetalFX exists on macOS only (vk_metalfx.mm); elsewhere it is never available.

#ifndef __APPLE__

#include "video_core/renderer_vulkan/vk_metalfx.h"

namespace Vulkan {

struct MetalFx::Impl {};

MetalFx::MetalFx() = default;
MetalFx::~MetalFx() = default;

std::unique_ptr<MetalFx> MetalFx::Create(const Instance&) {
    return nullptr;
}

void MetalFx::Prewarm(u32, u32, u32, u32, bool) {}

vk::Semaphore MetalFx::Semaphore() const {
    return {};
}

u64 MetalFx::NextReady() {
    return 0;
}

bool MetalFx::Encode(const Frame&, u64) {
    return false;
}

} // namespace Vulkan

#endif
