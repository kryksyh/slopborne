// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include <cstdio>
#include <xxhash.h>

#include "bbport_toggles.h"
#include "common/thread.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

AsyncCompiler::AsyncCompiler(u32 num_threads) {
    if (const char* env = std::getenv("BB_COMPILE_WORKERS"); env && std::atoi(env) > 0) {
        num_threads = u32(std::atoi(env));
    }
    for (u32 i = 0; i < num_threads; ++i) {
        threads.emplace_back([this](std::stop_token stop) { Worker(stop); });
    }
}

AsyncCompiler::~AsyncCompiler() {
    for (auto& thread : threads) {
        thread.request_stop();
    }
    cv.notify_all();
    threads.clear(); // joins; a running compile finishes first
}

void AsyncCompiler::Submit(std::function<void()>&& job) {
    submitted.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex};
        jobs.push_back(std::move(job));
    }
    cv.notify_one();
}

void AsyncCompiler::SubmitFront(std::function<void()>&& job) {
    submitted.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex};
        jobs.push_front(std::move(job));
    }
    cv.notify_one();
}

void AsyncCompiler::MarkWarmUpQueued() {
    warm_up.store(submitted.load(std::memory_order_relaxed), std::memory_order_release);
}

vk::ShaderModule AsyncCompiler::StandInModule(const Instance& instance, std::span<const u32> spirv) {
    const u64 hash = XXH3_64bits(spirv.data(), spirv.size_bytes());
    std::scoped_lock lock{modules_mutex};
    auto& module = stand_in_modules[hash];
    if (!module) {
        auto [result, created] = instance.GetDevice().createShaderModuleUnique(
            {.codeSize = spirv.size_bytes(), .pCode = spirv.data()});
        if (result != vk::Result::eSuccess) {
            return {};
        }
        module = std::move(created);
    }
    return *module;
}

SharedStandIn& AsyncCompiler::SharedStandInFor(u64 key_hash, bool& created) {
    std::scoped_lock lock{modules_mutex};
    auto& shared = shared_stand_ins[key_hash];
    created = !shared;
    if (created) {
        shared = std::make_unique<SharedStandIn>();
    }
    return *shared;
}

void AsyncCompiler::MaybeReport() {
    if (!BbStats::enabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report < std::chrono::seconds(5)) {
        return;
    }
    const u64 total = stats.stand_in_draws + stats.skipped_transparent +
                      stats.skipped_no_stand_in + stats.skipped_pending;
    if (total == last_report_total) {
        return;
    }
    last_report = now;
    last_report_total = total;
    size_t queued = 0;
    {
        std::scoped_lock lock{mutex};
        queued = jobs.size();
    }
    const auto avg = [](const std::atomic<u64>& sum, const std::atomic<u64>& count) {
        return count ? double(sum) / double(count) : 0.0;
    };
    std::printf("Async pipelines: %llu stand-in draws (%llu textured, %llu shared); skipped %llu transparent, "
                "%llu without stand-in, %llu stand-in pending; stand-in after %.0f ms avg, %llu max "
                "(%llu); ready after %.0f ms avg, %llu max (%llu); %zu compiles queued\n",
                static_cast<unsigned long long>(stats.stand_in_draws),
                static_cast<unsigned long long>(stats.textured_draws),
                static_cast<unsigned long long>(stats.shared_draws),
                static_cast<unsigned long long>(stats.skipped_transparent),
                static_cast<unsigned long long>(stats.skipped_no_stand_in),
                static_cast<unsigned long long>(stats.skipped_pending),
                avg(stats.stand_in_ms, stats.stand_ins),
                static_cast<unsigned long long>(stats.stand_in_max_ms),
                static_cast<unsigned long long>(stats.stand_ins), avg(stats.ready_ms, stats.readies),
                static_cast<unsigned long long>(stats.ready_max_ms),
                static_cast<unsigned long long>(stats.readies), queued);
}

void AsyncCompiler::Worker(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:PipelineCompile");
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lock{mutex};
            if (!cv.wait(lock, stop, [this] { return !jobs.empty(); })) {
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
        const u64 finished = done.fetch_add(1, std::memory_order_acq_rel) + 1;
        const u64 target = warm_up.load(std::memory_order_acquire);
        if (target && finished >= target && !reported.exchange(true)) {
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
            std::printf("Pipeline warm-up: %llu pipelines compiled %.1f s after start\n",
                        static_cast<unsigned long long>(target), seconds.count());
        }
    }
}


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
