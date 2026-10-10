// SPDX-License-Identifier: GPL-2.0-or-later
// A guest may reuse a consumed packet as soon as its write/fence becomes visible.
// Overwrite the active header deterministically, then verify decoding continues
// at the original next packet (also across an indirect-buffer coroutine).
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"

using namespace AmdGpu;

static u32* Write(u32* commands, u32* destination, u32 value) {
    const u64 address = reinterpret_cast<u64>(destination);
    return WritePacket<PM4ItOpcode::WriteData>(commands, PM4ShaderType::ShaderGraphics,
                                              5u << 8, u32(address), u32(address >> 32), value);
}

int main(int argc, char** argv) {
    Liverpool gpu;
    if (argc == 2 && std::strcmp(argv[1], "--truncated") == 0) {
        // A subprocess test expects the assertion and verifies BB_PM4_FAULT_DIR's
        // before/after snapshots. This must never silently consume the bad packet.
        std::array<u32, 3> truncated{PM4Type3Header{PM4ItOpcode::Nop, 17}.raw, 0x1234, 0x5678};
        gpu.SubmitGfx(truncated, {});
        gpu.WaitGpuIdle();
        return 2; // failure: malformed input was unexpectedly accepted
    }
    if (argc == 2 && std::strcmp(argv[1], "--mutated-truncated") == 0) {
        // Distinguish mutation during processing from malformed submitted input.
        u32 result = 0;
        std::array<u32, 10> changed{};
        auto* end = Write(changed.data(), changed.data() + 5,
                          PM4Type3Header{PM4ItOpcode::Nop, 17}.raw);
        end = Write(end, &result, 0x12345678);
        gpu.SubmitGfx({changed.data(), end}, {});
        gpu.WaitGpuIdle();
        return 2;
    }
    if (argc == 2 && (std::strcmp(argv[1], "--type0") == 0 ||
                      std::strcmp(argv[1], "--type1") == 0 ||
                      std::strcmp(argv[1], "--mutated-type0") == 0 ||
                      std::strcmp(argv[1], "--mutated-type1") == 0)) {
        const bool mutated = std::strstr(argv[1], "mutated") != nullptr;
        const u32 malformed = std::strstr(argv[1], "type1") ? 0x40000000u : 0u;
        u32 result = 0;
        std::array<u32, 10> commands{};
        if (mutated) {
            auto* end = Write(commands.data(), commands.data() + 5, malformed);
            end = Write(end, &result, 0x12345678);
            gpu.SubmitGfx({commands.data(), end}, {});
        } else {
            commands[0] = malformed;
            gpu.SubmitGfx({commands.data(), 2}, {});
        }
        gpu.WaitGpuIdle();
        return 2; // failure: unsupported input must still be rejected
    }
    const u32 replacement = PM4Type3Header{PM4ItOpcode::Nop, 17}.raw;
    // PM4's count field excludes two words of the total packet length. Verify
    // that neither the one-DWORD nor two-DWORD write touches its next neighbor.
    std::array<u32, 4> guarded{0xaaaaaaaa, 0, 0xbbbbbbbb, 0xcccccccc};
    std::array<u32, 8> sizes{};
    auto* size_end = Write(sizes.data(), &guarded[1], 0x11111111);
    assert(reinterpret_cast<const PM4CmdWriteData*>(sizes.data())->Size() == 4);
    gpu.SubmitGfx({sizes.data(), size_end}, {});
    gpu.WaitGpuIdle();
    assert((guarded == std::array<u32, 4>{0xaaaaaaaa, 0x11111111, 0xbbbbbbbb, 0xcccccccc}));
    const u64 target = reinterpret_cast<u64>(&guarded[1]);
    size_end = WritePacket<PM4ItOpcode::WriteData>(
        sizes.data(), PM4ShaderType::ShaderGraphics, 5u << 8, u32(target), u32(target >> 32),
        0x22222222u, 0x33333333u);
    assert(reinterpret_cast<const PM4CmdWriteData*>(sizes.data())->Size() == 8);
    gpu.SubmitGfx({sizes.data(), size_end}, {});
    gpu.WaitGpuIdle();
    assert((guarded == std::array<u32, 4>{0xaaaaaaaa, 0x22222222, 0x33333333, 0xcccccccc}));
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
    std::puts("PASS: exact WRITE_DATA payload bounds; graphics, indirect-parent and CE packet reuse");
}
