// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <mutex>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_info.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr address{};
    vk::Format format{};
    u32 width{}, height{};
    bool operator==(const Target&) const = default;
};

struct Entry {
    bool compute{};
    std::vector<Target> colors;
    Target depth;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs/ps pairs (graphics) or cs (compute), first few
    std::vector<std::pair<Target, bool>> sampled; ///< textures (false) and storage images (true)
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
};

// GPU thread only.
std::vector<Entry> entries;
std::vector<std::pair<Target, bool>> pending_sampled;
std::vector<std::string> pending_buffers;

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    pending_buffers.clear();
}
bool pass_open = false;
std::mutex display_mutex;
std::vector<VAddr> display_buffers;
// bbport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> display_list{};
std::atomic<u32> display_count{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height};
}

std::string Describe(const Target& t) {
    if (!t.address) {
        return "-";
    }
    return std::format("{:#x} {} {}x{}", t.address, vk::to_string(t.format), t.width, t.height);
}

void AddSampled(Entry& entry) {
    for (const auto& s : pending_sampled) {
        if (entry.sampled.size() < 32 &&
            std::ranges::find(entry.sampled, s) == entry.sampled.end()) {
            entry.sampled.push_back(s);
        }
    }
    pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 8 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

void Write(VAddr presented) {
    const char* dir = std::getenv("BB_CAPTURE_DIR");
    static u32 written = 0;
    const std::string path = std::format("{}/frame_{}_{:03}.txt", dir ? dir : ".",
                                         static_cast<long long>(std::time(nullptr)), written++);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    u32 index = 0;
    for (const auto& e : entries) {
        if (e.compute) {
            std::fprintf(f, "\n#%u COMPUTE dispatches %u, cs", index++, e.draws);
        } else {
            std::fprintf(f, "\n#%u PASS draws %u (indices %llu), vs/ps", index++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(f, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(f, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(f, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.compute) {
            std::fprintf(f, "  depth  %s\n", Describe(e.depth).c_str());
        }
        for (const auto& [t, storage] : e.sampled) {
            std::fprintf(f, "  %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(f, "  note: %s\n", e.note.c_str());
        }
        for (const auto& b : e.buffers) {
            std::fprintf(f, "%s\n", b.c_str());
        }
    }
    std::fclose(f);
    std::printf("Frame capture: %zu passes written to %s\n", entries.size(), path.c_str());
}

} // namespace

void FrameCapture::OnFlip(VAddr presented_address) {
    static const char* skip_path = std::getenv("BB_SKIP_FRAGMENT_FILE");
    if (skip_path) {
        u64 low = 1, high = 0;
        std::ifstream input(skip_path);
        if (!(input >> std::hex >> low >> high)) {
            low = 1;
            high = 0;
        }
        skip_fragment_low.store(low, std::memory_order_relaxed);
        skip_fragment_high.store(high, std::memory_order_relaxed);
    }
    last_presented.store(presented_address, std::memory_order_relaxed);
    const auto frame = flips.fetch_add(1, std::memory_order_release) + 1;
    static const u64 capture_at = [] {
        const char* value = std::getenv("BB_CAPTURE_AT");
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }();
    if (frame == capture_at && state.load(std::memory_order_relaxed) == Idle) {
        state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed at frame %llu\n", static_cast<unsigned long long>(frame));
    }
    // A monotonically increasing request number allows repeated captures without removing
    // a trigger file. Only acknowledge requests when a capture can actually be armed.
    static const char* sequence_path = std::getenv("BB_CAPTURE_SEQUENCE");
    static u64 last_sequence = 0;
    if (sequence_path && state.load(std::memory_order_relaxed) == Idle) {
        u64 sequence = 0;
        if (std::ifstream file(sequence_path); file >> sequence && sequence > last_sequence) {
            last_sequence = sequence;
            state.store(Armed, std::memory_order_release);
            std::printf("Frame capture: armed request %llu\n",
                        static_cast<unsigned long long>(sequence));
        }
    }
    static const char* trigger = std::getenv("BB_CAPTURE_TRIGGER");
    if (trigger && state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(trigger)) {
        std::error_code ec;
        std::filesystem::remove(trigger, ec);
        state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr address) {
    const u32 count = display_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        if (display_list[i].load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    if (count < display_list.size()) {
        return false;
    }
    std::scoped_lock lk{display_mutex};
    return std::ranges::find(display_buffers, address) != display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr address) {
    std::scoped_lock lk{display_mutex};
    if (std::ranges::find(display_buffers, address) == display_buffers.end()) {
        display_buffers.push_back(address);
        const u32 count = display_count.load(std::memory_order_relaxed);
        if (count < display_list.size()) {
            display_list[count].store(address, std::memory_order_relaxed);
            display_count.store(count + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                             const VideoCore::ImageInfo* depth) {
    std::vector<Target> targets;
    bool display = false;
    {
        std::scoped_lock lk{display_mutex};
        for (u32 i = 0; i < num_colors; ++i) {
            targets.push_back(colors[i] ? ToTarget(*colors[i]) : Target{});
            display |= colors[i] && std::ranges::find(display_buffers, colors[i]->guest_address) !=
                                        display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool new_pass = !(pass_open && !entries.empty() && !entries.back().compute &&
                            entries.back().colors == targets);
    if (display && new_pass) {
        if (state.load(std::memory_order_relaxed) == Recording) {
            Write(last_presented.load(std::memory_order_relaxed));
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            state.store(Idle, std::memory_order_release);
            return;
        }
        entries.clear();
        pending_sampled.clear();
        pending_buffers.clear();
        pass_open = false;
        state.store(Recording, std::memory_order_release);
    }
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    const Target d = depth ? ToTarget(*depth) : Target{};
    if (pass_open && !entries.empty() && !entries.back().compute &&
        entries.back().colors == targets && entries.back().depth == d) {
        return;
    }
    entries.push_back({.compute = false, .colors = std::move(targets), .depth = d});
    pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances) {
    if (state.load(std::memory_order_relaxed) != Recording || entries.empty() ||
        entries.back().compute) {
        return;
    }
    auto& e = entries.back();
    ++e.draws;
    e.indices += u64(num_indices) * std::max(num_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    AddSampled(e);
    AddBuffers(e);
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    if (!entries.empty() && entries.back().compute && !entries.back().shaders.empty() &&
        entries.back().shaders[0] == cs_hash) {
        ++entries.back().draws;
    } else {
        entries.push_back({.compute = true, .draws = 1, .shaders = {cs_hash}});
        entries.back().note = std::format("groups {}x{}x{}", x, y, z);
    }
    AddSampled(entries.back());
    AddBuffers(entries.back());
    pass_open = false;
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size) {
    const u64 bytes = std::min<u64>(size, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, address, size);
    const auto* words = static_cast<const float*>(data);
    for (u64 i = 0; i < bytes / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", words[i]);
    }
    pending_buffers.push_back(std::move(out));
}

namespace {
std::atomic<u32> gpu_buffer_captures{0};
constexpr u32 MaxGpuBufferCaptures = 512;
}

bool FrameCapture::WantsGpuBuffer(u64 stage_hash) {
    static const u64 selected = [] {
        const char* hash = std::getenv("BB_CAPTURE_GPU_BUFFER_HASH");
        const char* dir = std::getenv("BB_CAPTURE_GPU_BUFFER_DIR");
        if (!hash || !dir || !*dir) return u64{0};
        char* end{};
        const u64 value = std::strtoull(hash, &end, 16);
        return end && !*end ? value : u64{0};
    }();
    return selected && stage_hash == selected && Active() &&
           gpu_buffer_captures.load(std::memory_order_relaxed) < MaxGpuBufferCaptures;
}

void FrameCapture::GpuBuffer(Runtime& runtime, u64 stage_hash, u32 slot, VAddr address,
                             const VideoCore::Buffer* source, u64 offset, u64 full_size,
                             std::vector<u8> guest_bytes, const u8* staged_bytes) {
    if (!WantsGpuBuffer(stage_hash) || guest_bytes.empty() ||
        guest_bytes.size() > MaxGpuBufferBytes || offset > source->SizeBytes() ||
        guest_bytes.size() > source->SizeBytes() - offset)
        return;
    const u32 index = gpu_buffer_captures.fetch_add(1, std::memory_order_relaxed);
    if (index >= MaxGpuBufferCaptures) return;
    const u64 bytes = guest_bytes.size();
    std::vector<u8> staged;
    if (staged_bytes) staged.assign(staged_bytes, staged_bytes + bytes);
    const u64 frame = flips.load(std::memory_order_relaxed);
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = std::filesystem::path(std::getenv("BB_CAPTURE_GPU_BUFFER_DIR")) /
                      std::format("buffer_{}_{}_f{}_s{}", stamp, index, frame, slot);
    pending_buffers.push_back(std::format("  GPU/guest buffer pair: {}", path.string()));
    auto readback = std::make_shared<VideoCore::Buffer>(runtime.GetInstance(), 0, bytes,
                                                      VideoCore::MemoryType::HostCached,
                                                      "Diagnostic shader buffer readback");
    auto& scheduler = runtime.GetScheduler();
    const vk::BufferCopy copy{offset, 0, bytes};
    runtime.CopyBuffer(source, readback.get(), std::span{&copy, 1});
    // Keep the normal source-access tracker consistent, including later writes.
    runtime.FlushBarriers();
    scheduler.Record([](vk::CommandBuffer command) {
        const vk::MemoryBarrier2 host{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        command.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                   .pMemoryBarriers = &host});
    });
    const u64 arena_address = source->CpuAddr();
    scheduler.DeferPriorityOperation([readback, guest = std::move(guest_bytes), staged = std::move(staged), path, stage_hash,
                                       slot, address, offset, full_size, frame, arena_address] {
        readback->Invalidate(0, guest.size());
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        // An exclusive directory prevents overwriting any existing diagnostic.
        if (ec || !std::filesystem::create_directory(path, ec) || ec) {
            std::printf("GPU buffer capture: cannot create %s\n", path.string().c_str());
            return;
        }
        const auto gpu = readback->mapped_data.first(guest.size());
        u64 mismatches = 0, first_mismatch = guest.size();
        for (u64 i = 0; i < guest.size(); ++i) {
            if (gpu[i] != guest[i]) {
                ++mismatches;
                first_mismatch = std::min(first_mismatch, i);
            }
        }
        std::ofstream gpu_file(path / "gpu.bin", std::ios::binary);
        gpu_file.write(reinterpret_cast<const char*>(gpu.data()), gpu.size());
        std::ofstream guest_file(path / "guest.bin", std::ios::binary);
        guest_file.write(reinterpret_cast<const char*>(guest.data()), guest.size());
        u64 staged_mismatches = 0;
        bool staged_written = true;
        if (!staged.empty()) {
            std::ofstream staged_file(path / "staged.bin", std::ios::binary);
            staged_file.write(reinterpret_cast<const char*>(staged.data()), staged.size());
            staged_written = bool(staged_file);
            for (u64 i = 0; i < staged.size(); ++i) staged_mismatches += staged[i] != gpu[i];
        }
        std::ofstream metadata(path / "metadata.txt");
        metadata << std::format("shader={:016x}\nslot={}\nflip={}\nguest_address={:#x}\n"
                                "arena_address={:#x}\nsource_offset={}\nfull_bytes={}\n"
                                "captured_bytes={}\nmismatched_bytes={}\nfirst_mismatch={}\n",
                                stage_hash, slot, frame, address, arena_address, offset, full_size,
                                guest.size(), mismatches, first_mismatch);
        metadata << std::format("staged_capture={}\ngpu_staged_mismatched_bytes={}\n",
                                !staged.empty(), staged_mismatches);
        metadata << "Guest bytes were sampled at binding time. GPU-generated inputs may legitimately "
                    "differ. The copy and barriers perturb timing; this is not FPS evidence.\n";
        if (!gpu_file || !guest_file || !metadata || !staged_written)
            std::printf("GPU buffer capture: incomplete write at %s\n", path.string().c_str());
        else
            std::printf("GPU buffer capture: %s (%llu/%zu bytes differ)\n", path.string().c_str(),
                        static_cast<unsigned long long>(mismatches), guest.size());
    });
}

void FrameCapture::Sampled(const VideoCore::ImageInfo& info, bool storage) {
    if (pending_sampled.size() < 64) {
        pending_sampled.emplace_back(ToTarget(info), storage);
    }
}

void FrameCapture::Note(const char* text) {
    if (!entries.empty()) {
        entries.back().note += text;
    }
}

bool FrameCapture::WantsDrawInputs(u64 vs_hash, u64 ps_hash) {
    static const u64 selected = [] {
        const char* hash = std::getenv("BB_CAPTURE_DRAW_HASH");
        if (!hash || !*hash) return u64{0};
        char* end{};
        const u64 value = std::strtoull(hash, &end, 16);
        return end && !*end ? value : u64{0};
    }();
    return selected && (vs_hash == selected || ps_hash == selected) && Active();
}

void FrameCapture::DrawInputs(u64 vs_hash, u64 ps_hash, const Shader::PushData& push,
                               std::span<const vk::WriteDescriptorSet> writes) {
    if (!WantsDrawInputs(vs_hash, ps_hash) ||
        state.load(std::memory_order_relaxed) != Recording || entries.empty()) return;
    std::string text = std::format("\n  exact inputs vs {:016x} ps {:016x}\n  push bytes:",
                                   vs_hash, ps_hash);
    // Preserve float bit patterns, offsets and every packed user-data field.
    for (const auto byte : std::as_bytes(std::span{&push, 1})) {
        text += std::format(" {:02x}", std::to_integer<u8>(byte));
    }
    for (const auto& write : writes) {
        text += std::format("\n  descriptor binding {} array {} type {} count {}",
                            write.dstBinding, write.dstArrayElement,
                            vk::to_string(write.descriptorType), write.descriptorCount);
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            if (write.pBufferInfo) {
                const auto& info = write.pBufferInfo[i];
                text += std::format("\n    [{}] buffer {:#x} offset {} range {}", i,
                                    std::bit_cast<u64>(info.buffer), info.offset, info.range);
            }
            if (write.pImageInfo) {
                const auto& info = write.pImageInfo[i];
                text += std::format("\n    [{}] image_view {:#x} sampler {:#x} layout {}", i,
                                    std::bit_cast<u64>(info.imageView),
                                    std::bit_cast<u64>(info.sampler),
                                    vk::to_string(info.imageLayout));
            }
            if (write.pTexelBufferView) {
                text += std::format("\n    [{}] texel_view {:#x}", i,
                                    std::bit_cast<u64>(write.pTexelBufferView[i]));
            }
        }
    }
    Note(text.c_str());
}

} // namespace Vulkan
