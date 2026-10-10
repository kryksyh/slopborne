// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame analyzer. Incrementing the number in BB_CAPTURE_SEQUENCE records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in Bloodborne's frame.
// BB_CAPTURE_AT records one numbered frame. The legacy BB_CAPTURE_TRIGGER consumes its file.

#pragma once

#include <atomic>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"

namespace Shader {
struct PushData;
}
namespace vk {
struct WriteDescriptorSet;
}

namespace VideoCore {
struct ImageInfo;
struct Buffer;
}

namespace Vulkan {
class Runtime;

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// Optional diagnostic hash interval, read from BB_SKIP_FRAGMENT_FILE on each flip.
    static bool SkipFragment(u64 hash) {
        return hash != 0 && hash >= skip_fragment_low.load(std::memory_order_relaxed) &&
               hash <= skip_fragment_high.load(std::memory_order_relaxed);
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool storage);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    // Opt-in coherence diagnostic: capture GPU bytes and binding-time guest bytes
    // without waiting on the GPU thread. Restrict to one shader and active captures.
    static bool WantsGpuBuffer(u64 stage_hash);
    static constexpr u64 MaxGpuBufferBytes = 64 * 1024;
    static void GpuBuffer(Runtime& runtime, u64 stage_hash, u32 slot, VAddr address,
                          const VideoCore::Buffer* source, u64 offset, u64 full_size,
                          std::vector<u8> guest_bytes, const u8* staged_bytes = nullptr);
    static void Note(const char* text);
    /// BB_CAPTURE_DRAW_HASH: exact host-side values sent for a matching direct draw.
    static bool WantsDrawInputs(u64 vs_hash, u64 ps_hash);
    static void DrawInputs(u64 vs_hash, u64 ps_hash, const Shader::PushData& push,
                           std::span<const vk::WriteDescriptorSet> writes);

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
    static inline std::atomic<u64> skip_fragment_low{1};
    static inline std::atomic<u64> skip_fragment_high{0};
};

} // namespace Vulkan
