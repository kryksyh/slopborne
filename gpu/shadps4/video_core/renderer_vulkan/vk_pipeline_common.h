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
#include <thread>
#include <vector>

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

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

private:
    void Worker(std::stop_token stop);

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
