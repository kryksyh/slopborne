// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise actual committed arena migration and GPU readback, without game files.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {
struct DenseArenaTest {
    static void Run(BufferCache& cache, Vulkan::Runtime& runtime,
                    Vulkan::Scheduler& scheduler, const Vulkan::Instance& instance) {
        assert(!cache.sparse_buffers);
        const u64 page = cache.arena_page_size;
        const VAddr base = 0x7000000000ULL;
        const auto obtain = [&](VAddr address, u64 size) {
            return cache.GetDenseArena(address >> cache.block_shift,
                                       (address + size - 1) >> cache.block_shift);
        };
        const auto* left = obtain(base, 4);
        const auto* right = obtain(base + 2 * page, page + 4);
        cache.EnsureResident(left, base >> cache.block_shift, base >> cache.block_shift);
        cache.EnsureResident(right, (base + 2 * page) >> cache.block_shift,
                              (base + 2 * page) >> cache.block_shift);
        runtime.FillBuffer(left, 128, 4, 0x12345678);
        runtime.FillBuffer(right, 256, 4, 0xabcdef01);

        // A descriptor prepared before another binding triggers the merge.
        vk::DescriptorBufferInfo descriptor{right->Handle(), 256, 4};
        const Buffer* pointer = left;
        u64 offset = 128;
        const auto* merged = obtain(base + page - 4, page + 8);
        assert(merged->cpu_addr == base && merged->size_bytes == 4 * page);
        assert(cache.RefreshBinding(descriptor.buffer, descriptor.offset));
        assert(descriptor.buffer == merged->Handle() && descriptor.offset == 2 * page + 256);
        assert(cache.RefreshBinding(pointer, offset));
        assert(pointer == merged && offset == 128);
        runtime.FillBuffer(pointer, offset, 4, 0x87654321);

        // Grow to the left; both an original descriptor and a once-repaired one
        // must map to the latest arena, with the newly shifted offsets.
        auto original_handle = right->Handle();
        u64 original_offset = 256;
        const auto* final = obtain(base - 4, 8);
        assert(final->cpu_addr == base - page && final->size_bytes == 5 * page);
        assert(cache.RefreshBinding(original_handle, original_offset));
        assert(original_handle == final->Handle() && original_offset == 3 * page + 256);
        assert(cache.RefreshBinding(descriptor.buffer, descriptor.offset));
        assert(descriptor.buffer == final->Handle() && descriptor.offset == original_offset);
        assert(cache.RefreshBinding(pointer, offset));
        assert(pointer == final && offset == page + 128);
        assert(!cache.RefreshBinding(pointer, offset));
        assert(obtain(base + page, 4) == final);

        Buffer readback(instance, 0, 64, MemoryType::HostCached);
        const vk::BufferCopy data_copies[] = {{offset, 0, 4}, {original_offset, 4, 4},
                                              {2 * page, 8, 4}};
        runtime.CopyBuffer(final, &readback, data_copies);
        const vk::BufferCopy bda_copies[] = {
            {(base >> cache.block_shift) * 8, 16, 8},
            {((base + 2 * page) >> cache.block_shift) * 8, 24, 8},
            {((base + page) >> cache.block_shift) * 8, 32, 8}};
        runtime.CopyBuffer(cache.GetBdaPageTableBuffer(), &readback, bda_copies);
        runtime.FlushBarriers();
        scheduler.Finish();
        readback.Invalidate(0, 64);
        u32 words[3];
        u64 addresses[3];
        std::memcpy(words, readback.mapped_data.data(), sizeof(words));
        std::memcpy(addresses, readback.mapped_data.data() + 16, sizeof(addresses));
        assert(words[0] == 0x87654321 && words[1] == 0xabcdef01 && words[2] == 0);
        assert(addresses[0] == final->BufferDeviceAddress() + page);
        assert(addresses[1] == final->BufferDeviceAddress() + 3 * page);
        assert(addresses[2] == 0); // Nonresident holes remain faultable.
        std::puts("PASS: dense arena overlap, GPU data, repeated rebinding and BDA relocation");
    }
};

struct Caches {
    PageManager pages{nullptr};
    BufferCache buffers;
    TextureCache textures;
    Caches(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
           Vulkan::Runtime& runtime)
        : buffers(instance, scheduler, runtime, nullptr, textures, pages),
          textures(instance, scheduler, runtime, nullptr, buffers, pages) {}
};
} // namespace VideoCore

int main() {
    Vulkan::Instance instance(0, false);
    if (instance.SupportsSparseBuffers()) {
        std::puts("SKIP: requires the committed-buffer fallback");
        return 0;
    }
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    VideoCore::Caches caches(instance, scheduler, runtime);
    VideoCore::DenseArenaTest::Run(caches.buffers, runtime, scheduler, instance);
}
