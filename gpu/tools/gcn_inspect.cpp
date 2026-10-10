// SPDX-License-Identifier: GPL-2.0-or-later
// Offline inspection of trusted, extracted GCN machine code. No Vulkan device or guest process.
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <vector>

#include "shader_recompiler/frontend/decode.h"

template <>
struct magic_enum::customize::enum_range<Shader::Gcn::OperandField> {
    static constexpr int min = 0;
    static constexpr int max = 256;
};

using namespace Shader::Gcn;

static void PrintOperand(const InstOperand& op) {
    std::cout << magic_enum::enum_name(op.field) << ':' << std::hex << op.code << std::dec;
    if (op.input_modifier.abs) std::cout << "[abs]";
    if (op.input_modifier.neg) std::cout << "[neg]";
    if (op.output_modifier.clamp) std::cout << "[clamp]";
    if (op.output_modifier.multiplier != 0.f) std::cout << "[mul=" << op.output_modifier.multiplier << ']';
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: gcn-inspect raw-gcn.bin\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    const auto bytes = input ? static_cast<std::streamoff>(input.tellg()) : -1;
    if (bytes <= 0 || bytes % 4 || bytes > 16 * 1024 * 1024) {
        std::cerr << "Expected a nonempty DWORD-aligned shader, at most 16 MiB\n";
        return 2;
    }
    const size_t count = static_cast<size_t>(bytes) / sizeof(u32);
    // Decoder reads operands eagerly; padding makes a final truncated instruction safe to detect.
    std::vector<u32> words(count + 4);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(words.data()), bytes)) {
        std::cerr << "Could not read shader\n";
        return 2;
    }
    GcnCodeSlice code(words.data(), words.data() + count);
    GcnDecodeContext decoder;
    std::map<std::string, size_t> histogram;
    size_t instructions = 0;
    while (!code.atEnd()) {
        const auto start = code.position();
        const auto inst = decoder.decodeInstruction(code);
        if (code.position() <= start || code.position() > words.data() + count) {
            std::cerr << "Truncated instruction at byte " << (start - words.data()) * 4 << '\n';
            return 1;
        }
        const std::string opcode(magic_enum::enum_name(inst.opcode));
        ++histogram[opcode];
        ++instructions;
        std::cout << std::hex << std::setw(6) << std::setfill('0')
                  << (start - words.data()) * 4 << std::dec << "  " << opcode;
        for (u32 i = 0; i < inst.dst_count; ++i) {
            std::cout << (i ? ", " : " dst=");
            PrintOperand(inst.dst[i]);
        }
        for (u32 i = 0; i < inst.src_count; ++i) {
            std::cout << (i ? ", " : " src=");
            PrintOperand(inst.src[i]);
        }
        if (inst.encoding == InstEncoding::SOPP) {
            std::cout << " simm=" << inst.control.sopp.simm;
        } else if (inst.encoding == InstEncoding::SOPK) {
            std::cout << " simm=" << inst.control.sopk.simm;
        } else if (inst.encoding == InstEncoding::SMRD) {
            std::cout << " offset=" << inst.control.smrd.offset << " imm=" << inst.control.smrd.imm;
        } else if (inst.encoding == InstEncoding::MUBUF) {
            const auto& c = inst.control.mubuf;
            std::cout << " offset=" << c.offset << " offen=" << c.offen << " idxen=" << c.idxen
                      << " glc=" << c.glc << " slc=" << c.slc << " count=" << c.count;
        } else if (inst.encoding == InstEncoding::EXP) {
            const auto& c = inst.control.exp;
            std::cout << " target=" << c.target << " en=" << c.en << " done=" << c.done;
        }
        std::cout << " ; raw";
        for (auto p = start; p != code.position(); ++p) std::cout << ' ' << std::hex << std::setw(8) << *p;
        std::cout << std::dec << '\n';
    }
    std::cout << "\n# " << instructions << " instructions, " << bytes << " bytes\n";
    for (const auto& [opcode, n] : histogram) {
        std::cout << "# " << opcode << ' ' << n << '\n';
    }
}
