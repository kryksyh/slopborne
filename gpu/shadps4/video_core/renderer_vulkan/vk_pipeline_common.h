// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

class Instance;

/// bbport: a grey stand-in pipeline shared by every pipeline with the same stages but the
/// fragment shader and the same render state: compiled once, so a new material drawn with a
/// known vertex shader has a stand-in at once.
struct SharedStandIn {
    vk::UniquePipeline pipeline;
    std::atomic<bool> ready{false};
};

/// bbport: driver pipeline compiles (MoltenVK: the Metal compile, 10-200 ms) off the GPU command
/// thread. A pipeline is published to draws once its compile finished (Pipeline::IsReady);
/// until then its draws are skipped instead of stalling the frame.
class AsyncCompiler {
public:
    explicit AsyncCompiler(u32 num_threads);
    ~AsyncCompiler();
    void Submit(std::function<void()>&& job);
    /// Ahead of the queued compiles (a stand-in a draw is waiting for).
    void SubmitFront(std::function<void()>&& job);
    /// The warm-up queued all its compiles: reported once they are done.
    void MarkWarmUpQueued();

    /// A stand-in fragment module per distinct SPIR-V: pipelines sharing one reuse the driver's
    /// compiled library. Null when the module could not be created.
    vk::ShaderModule StandInModule(const Instance& instance, std::span<const u32> spirv);
    /// The shared stand-in for `key_hash`; `created` when this call made it (its compile is
    /// then the caller's to queue).
    SharedStandIn& SharedStandInFor(u64 key_hash, bool& created);

    /// Draws without their pipeline, by outcome, and how long pipelines took to get a stand-in
    /// and to be ready (ms after the pipeline was queued).
    struct Stats {
        std::atomic<u64> stand_in_draws{}, textured_draws{}, shared_draws{}, skipped_transparent{},
            skipped_no_stand_in{}, skipped_pending{};
        std::atomic<u64> stand_ins{}, stand_in_ms{}, stand_in_max_ms{}, readies{}, ready_ms{},
            ready_max_ms{};
        void NoteStandIn(u64 ms) {
            ++stand_ins;
            stand_in_ms += ms;
            Max(stand_in_max_ms, ms);
        }
        void NoteReady(u64 ms) {
            ++readies;
            ready_ms += ms;
            Max(ready_max_ms, ms);
        }
        static void Max(std::atomic<u64>& target, u64 value) {
            u64 current = target.load(std::memory_order_relaxed);
            while (current < value && !target.compare_exchange_weak(current, value)) {
            }
        }
    } stats;
    /// Prints the stats (BB_FRAME_STATS=1) at most every 5 s, when draws went without a pipeline.
    void MaybeReport();

private:
    void Worker(std::stop_token stop);

    std::mutex modules_mutex;
    std::unordered_map<u64, vk::UniqueShaderModule> stand_in_modules;
    std::unordered_map<u64, std::unique_ptr<SharedStandIn>> shared_stand_ins;
    std::chrono::steady_clock::time_point last_report{};
    u64 last_report_total{};

    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::function<void()>> jobs;
    std::vector<std::jthread> threads;
    // Reports when the compiles the warm-up queued are done.
    std::atomic<u64> submitted{0}, done{0}, warm_up{0};
    std::atomic<bool> reported{false};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    /// bbport: false while the driver compile runs on the AsyncCompiler.
    bool IsReady() const noexcept {
        return ready.load(std::memory_order_acquire);
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    std::atomic<bool> ready{true};
};

} // namespace Vulkan
