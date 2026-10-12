// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <optional>
#include <tuple>
#include <boost/container/small_vector.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv_stand_in.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {
namespace {

bool IsSample(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleExplicitLod;
}

/// The sample instruction nearest to `value` through its operands, if any within a few steps.
const IR::Inst* FindSample(const IR::Value& value, u32& depth_out) {
    boost::container::small_vector<std::pair<const IR::Inst*, u32>, 64> queue;
    if (const auto* inst = value.TryInst()) {
        queue.emplace_back(inst, 0);
    }
    for (size_t at = 0; at < queue.size(); ++at) {
        const auto [inst, depth] = queue[at];
        if (IsSample(inst->GetOpcode())) {
            depth_out = depth;
            return inst;
        }
        if (depth >= 12 || queue.size() >= 64 || inst->GetOpcode() == IR::Opcode::Phi) {
            continue;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            const auto* arg = inst->Arg(i).TryInst();
            if (arg && std::ranges::find(queue, arg, &std::pair<const IR::Inst*, u32>::first) ==
                           queue.end()) {
                queue.emplace_back(arg, depth + 1);
            }
        }
    }
    return nullptr;
}

/// The interpolant component a texture coordinate comes from, through at most a few
/// arithmetic operations (UV scale and offset).
std::optional<std::pair<IR::Attribute, u32>> FindInterpolant(const IR::Value& value,
                                                             u32 depth = 0) {
    const auto* inst = value.TryInst();
    if (!inst || depth > 4) {
        return std::nullopt;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttribute: {
        const auto attribute = inst->Arg(0).Attribute();
        if (!IR::IsParam(attribute) || inst->Arg(2).U32() != 0) {
            return std::nullopt;
        }
        return std::pair{attribute, inst->Arg(1).U32()};
    }
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPFma32:
    case IR::Opcode::FPNeg32:
    case IR::Opcode::FPSaturate32:
    case IR::Opcode::FPClamp32:
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            if (const auto found = FindInterpolant(inst->Arg(i), depth + 1)) {
                return found;
            }
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

/// The texture and interpolant behind `sample`, if a stand-in can reproduce it.
std::optional<Info::StandIn::Target> ResolveSample(const EmitContext& ctx, const Info& info,
                                                   const RuntimeInfo& runtime_info,
                                                   const IR::Inst& sample) {
    const u32 handle = sample.Arg(0).U32();
    const u32 image = handle & 0xFFFF;
    const u32 sampler = handle >> 16;
    if (image >= ctx.images.size() || sampler >= ctx.sampler_bindings.size()) {
        return std::nullopt;
    }
    const auto& texture = ctx.images[image];
    if (texture.view_type != AmdGpu::ImageType::Color2D || texture.is_integer ||
        texture.is_storage || texture.mip_fallback_mode != MipStorageFallbackMode::None ||
        info.images[image].is_depth) {
        return std::nullopt;
    }
    const auto* coords = sample.Arg(1).TryInst();
    if (!coords || coords->GetOpcode() != IR::Opcode::CompositeConstructF32x2) {
        return std::nullopt;
    }
    const auto u = FindInterpolant(coords->Arg(0));
    const auto v = FindInterpolant(coords->Arg(1));
    if (!u || !v || u->first != v->first) {
        return std::nullopt;
    }
    const u32 param = u32(u->first) - u32(IR::Attribute::Param0);
    const auto& fs = runtime_info.hw.fs;
    if (param >= fs.num_inputs || fs.inputs[param].IsDefault() ||
        info.fs_interpolation[param].primary == Qualifier::PerVertex) {
        return std::nullopt;
    }
    return Info::StandIn::Target{
        .valid = true,
        .image_binding = texture.unified_binding,
        .sampler_binding = ctx.sampler_bindings[sampler],
        .location = u32(fs.inputs[param].param_index) + (fs.clip_distance_emulation ? 1u : 0u),
        .uv_comps = {u8(u->second), u8(v->second)},
    };
}

} // namespace

void RecordStandIn(const EmitContext& ctx, const IR::Program& program,
                   const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    info.stand_in = {};
    info.stand_in.alpha_test = info.has_discard;
    // Per render target, the sample reached from most of its color components, and among
    // those through the fewest operations.
    struct Candidate {
        u32 target;
        const IR::Inst* sample;
        u32 comps;
        u32 depth;
    };
    boost::container::small_vector<Candidate, 8> candidates;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute ||
                !IR::IsMrt(inst.Arg(0).Attribute()) || inst.Arg(2).U32() > 2) {
                continue;
            }
            const u32 target = u32(inst.Arg(0).Attribute()) - u32(IR::Attribute::RenderTarget0);
            u32 depth = 0;
            const auto* sample = FindSample(inst.Arg(1), depth);
            if (!sample) {
                continue;
            }
            const auto it = std::ranges::find_if(candidates, [&](const Candidate& c) {
                return c.target == target && c.sample == sample;
            });
            if (it == candidates.end()) {
                candidates.push_back({target, sample, 1, depth});
            } else {
                ++it->comps;
                it->depth += depth;
            }
        }
    }
    for (u32 target = 0; target < IR::NumRenderTargets; ++target) {
        const Candidate* best = nullptr;
        for (const auto& candidate : candidates) {
            if (candidate.target == target &&
                (!best || std::tie(candidate.comps, best->depth) >
                              std::tie(best->comps, candidate.depth))) {
                best = &candidate;
            }
        }
        if (!best) {
            continue;
        }
        if (const auto resolved = ResolveSample(ctx, info, runtime_info, *best->sample)) {
            info.stand_in.targets[target] = *resolved;
        }
    }
}

} // namespace Shader::Backend::SPIRV
