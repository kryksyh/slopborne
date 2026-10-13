// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <boost/container/static_vector.hpp>

#include <unordered_map>
#include <boost/icl/interval_set.hpp>
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class GpuProfiler;
class SceneTargets;

class Instance;
class Scheduler;

class Runtime {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime() = default;

    Scheduler& GetScheduler() {
        return scheduler;
    }

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();
    SceneTargets* scene_targets = nullptr;

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value);

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);

    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {});

    /// BB_GPU_PROFILE: a profiler segment for a transfer of `image` while it lives; the
    /// segment it interrupted continues afterwards.
    class TransferMark {
    public:
        TransferMark(Runtime& runtime, const char* what, const VideoCore::Image& image);
        ~TransferMark();
        TransferMark(const TransferMark&) = delete;
        TransferMark& operator=(const TransferMark&) = delete;

    private:
        GpuProfiler* profiler = nullptr;
        u64 resume = 0;
    };
    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);

    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false);

    void FlushBarriers();

    /// bbport: the pending memory barrier as part of the barriers ahead of the current render
    /// pass (no split) when the pass itself wrote nothing; false when it does not apply.
    bool FlushBarriersBeforePass();

    /// bbport: images attached by the current render pass (set with the pass state): layout
    /// transitions of any other image can be recorded ahead of the pass instead of splitting it.
    boost::container::static_vector<vk::Image, 9> pass_images;

    /// bbport: notes a buffer range read or written by commands of the current submission.
    void TouchBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size, bool write = false);

    /// bbport: a vertex buffer range read by the current draw.
    void TouchPassRead(vk::Buffer buffer, u64 offset, u64 size);

    /// bbport: a draw begins noting its accesses (see draw_touches).
    void MarkDrawStart();

    /// bbport: commands of this submission access buffers TouchBuffer cannot name.
    void TouchUntracked() {
        SyncSubmission();
        submission_untracked = true;
    }

    /// bbport: while a render pass is open, records `copies` into the upload prologue instead
    /// (the pass is not split) when no command of this submission has touched the
    /// destination ranges yet. False when the copy must stay in order.
    /// `src_tracked`: the source may also be written by commands (not a staging buffer).
    bool CopyBufferBeforePass(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                              std::span<const vk::BufferCopy> copies, bool src_tracked = false);

    /// bbport: runs before this thread changes image state (layouts, pending image
    /// barriers): the rasterizer joins its texture binding helper there (BindHelper).
    void SetImageAccessHook(void (*hook)(void*), void* context) {
        image_access_hook = hook;
        image_access_context = context;
    }
    void BeforeImageAccess() {
        if (image_access_hook) {
            image_access_hook(image_access_context);
        }
    }

private:
    void (*image_access_hook)(void*) = nullptr;
    void* image_access_context = nullptr;
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    BarrierTracker barrier_tracker;
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
    // bbport: ranges inserted into barrier_tracker since its last Clear(); re-inserting a
    // contained range is a no-op, and draws re-bind the same ranges constantly.
    struct AccessMemo {
        u64 resource;
        u64 start;
        u64 end;
        u32 epoch;
        u32 access;
    };
    std::array<AccessMemo, 512> access_memo{};
    u32 access_epoch = 1;
    // bbport: buffer ranges touched by commands of the submission being recorded.
    std::unordered_map<u64, boost::icl::interval_set<u64>> submission_ranges;
    u64 submission_tick = ~0ull;
    bool submission_untracked = false;
    // Exact ranges read (of watched buffers) and written since the current render pass began,
    // by VkBuffer: may a copy go before the pass? The current draw's accesses are noted before
    // its pass begins, so they wait in draw_touches until the next draw starts.
    struct Touch {
        u64 buffer, start, end;
        bool write;
    };
    std::unordered_map<u64, boost::icl::interval_set<u64>> pass_reads, pass_writes;
    std::vector<Touch> draw_touches;
    std::unordered_map<u64, u64> pass_watched; ///< VkBuffer -> pass serial it was added in
    u64 pass_ranges_serial = ~0ull;
    void SyncPassRanges();
    void TouchPass(u64 buffer, u64 start, u64 end, bool write);
    bool CopyBufferBeforeRenderPass(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                                    std::span<const vk::BufferCopy> copies, bool src_tracked);
    void SyncSubmission();
};

} // namespace Vulkan
