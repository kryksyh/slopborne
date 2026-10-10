// SPDX-License-Identifier: GPL-2.0-or-later
// A guest may reuse a consumed packet as soon as its write/fence becomes visible.
// Overwrite the active header deterministically, then verify decoding continues
// at the original next packet (also across an indirect-buffer coroutine).
#include <array>
#include <cassert>
#include <cstdio>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"

using namespace AmdGpu;

static u32* Write(u32* commands, u32* destination, u32 value) {
    const u64 address = reinterpret_cast<u64>(destination);
    return WritePacket<PM4ItOpcode::WriteData>(commands, PM4ShaderType::ShaderGraphics,
                                              5u << 8, u32(address), u32(address >> 32), value);
}

int main() {
    Liverpool gpu;
    const u32 replacement = PM4Type3Header{PM4ItOpcode::Nop, 17}.raw;
    u32 result = 0;
    std::array<u32, 16> commands{};
    auto* end = Write(commands.data(), commands.data(), replacement);
    end = Write(end, &result, 0x12345678);
    gpu.SubmitGfx({commands.data(), end}, {});
    gpu.WaitGpuIdle();
    assert(commands[0] == replacement);
    assert(result == 0x12345678);

    result = 0;
    std::array<u32, 8> child{};
    const auto* child_end = Write(child.data(), commands.data(), replacement);
    const u64 child_address = reinterpret_cast<u64>(child.data());
    end = WritePacket<PM4ItOpcode::IndirectBuffer>(
        commands.data(), PM4ShaderType::ShaderGraphics, u32(child_address),
        u32(child_address >> 32), u32(child_end - child.data()));
    end = Write(end, &result, 0xabcdef01);
    gpu.SubmitGfx({commands.data(), end}, {});
    gpu.WaitGpuIdle();
    assert(commands[0] == replacement);
    assert(result == 0xabcdef01);
    // CE memory dumps can release/rewrite their own packet in the same way.
    result = 0;
    std::array<u32, 24> constants{};
    auto* ce_end = WritePacket<PM4ItOpcode::WriteConstRam>(
        constants.data(), PM4ShaderType::ShaderGraphics, 0u, replacement);
    const u64 dump_header = reinterpret_cast<u64>(ce_end);
    ce_end = WritePacket<PM4ItOpcode::DumpConstRam>(
        ce_end, PM4ShaderType::ShaderGraphics, 0u, 1u, u32(dump_header), u32(dump_header >> 32));
    ce_end = WritePacket<PM4ItOpcode::WriteConstRam>(
        ce_end, PM4ShaderType::ShaderGraphics, 0u, 0x76543210u);
    const u64 destination = reinterpret_cast<u64>(&result);
    ce_end = WritePacket<PM4ItOpcode::DumpConstRam>(
        ce_end, PM4ShaderType::ShaderGraphics, 0u, 1u, u32(destination), u32(destination >> 32));
    gpu.SubmitGfx({}, {constants.data(), ce_end});
    gpu.WaitGpuIdle();
    assert(result == 0x76543210);
    std::puts("PASS: graphics, indirect-parent and constant-engine PM4 packet reuse");
}
