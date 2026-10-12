// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Shader {
struct RuntimeInfo;
namespace IR {
struct Program;
}
} // namespace Shader

namespace Shader::Backend::SPIRV {

class EmitContext;

/// bbport: records in the fragment program's Info what a textured stand-in can show while the
/// pipeline compiles in the background (Info::StandIn): the 2D texture whose sample feeds
/// render target 0, and the interpolant it is sampled with.
void RecordStandIn(const EmitContext& ctx, const IR::Program& program,
                   const RuntimeInfo& runtime_info);

} // namespace Shader::Backend::SPIRV
