// SPDX-License-Identifier: GPL-2.0-or-later
// Offline regeneration of a pipeline-cache shader permutation: translates the GCN program found in
// the game's files (scripts/game_shaders.py --out) with the specialization stored in the cache's
// .meta, as PipelineCache::CompileModule does in the game. No Vulkan device or guest process.
// Usage: shader-regen <cache dir> <program dir> <name>.meta [out.spv]
//   exit 0: regenerated SPIR-V equals the cached <hash>_<perm>.spv (or was written to out.spv)
//   exit 1: differs; 2: input missing or unreadable.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"

namespace Vulkan {
bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx);
}

namespace fs = std::filesystem;

static std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: shader-regen <cache dir> <program dir> <name>.meta [out.spv]\n";
        return 2;
    }
    const fs::path cache = argv[1], programs = argv[2];
    const auto profile_bytes = ReadFile(cache / "profile.bin");
    Shader::Profile profile{};
    if (profile_bytes.size() != sizeof(profile)) {
        std::cerr << "profile.bin: unexpected size\n";
        return 2;
    }
    std::memcpy(&profile, profile_bytes.data(), sizeof(profile));

    Serialization::Archive meta_ar{ReadFile(cache / argv[3])};
    Shader::Info stored{};
    Shader::StageSpecialization spec{};
    spec.info = &stored;
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    size_t perm_idx{};
    if (!Vulkan::LoadShaderMeta(meta_ar, stored, fetch, spec, perm_idx)) {
        std::cerr << argv[3] << ": unsupported meta\n";
        return 2;
    }
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%#018llx", static_cast<unsigned long long>(stored.pgm_hash));
    const auto code_bytes = ReadFile(programs / (std::string{hash} + ".bin"));
    if (code_bytes.empty() || code_bytes.size() % 4) {
        std::cerr << hash << ": program not found in the game files\n";
        return 2;
    }
    std::vector<u32> code(code_bytes.size() / 4);
    std::memcpy(code.data(), code_bytes.data(), code_bytes.size());

    // The registers the game set for this draw: the first dwords of the stored flattened data.
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::memcpy(user_data.data(), stored.flattened_ud_buf.data(),
                std::min(sizeof(user_data), stored.flattened_ud_buf.size() * sizeof(u32)));
    // The fetch shader the game generated for this permutation (stored with the cache entry):
    // the program reads it through the user data register pair that held its guest address.
    std::vector<u32> fetch_code;
    if (stored.has_fetch_shader) {
        const auto bytes = ReadFile(cache / (std::string{hash} + "_" + std::to_string(perm_idx) + ".fetch"));
        if (bytes.empty() || bytes.size() % 4) {
            std::cerr << hash << '_' << perm_idx << ": fetch shader not stored\n";
            return 2;
        }
        fetch_code.resize(bytes.size() / 4 + 4); // padding: the decoder reads ahead
        std::memcpy(fetch_code.data(), bytes.data(), bytes.size());
        const auto address = reinterpret_cast<u64>(fetch_code.data());
        std::memcpy(&user_data[stored.fetch_shader_sgpr_base], &address, sizeof(address));
    }
    const Shader::ShaderParams params{.user_data = user_data, .code = code, .hash = stored.pgm_hash};
    Shader::Info info{stored.hw_stage, stored.sw_stage, params};
    info.flattened_ud_buf = stored.flattened_ud_buf;

    std::fprintf(stderr, "%s_%zu: hw stage %d, sw stage %d, fetch %d, flat user data %zu dwords\n",
                 hash, perm_idx, int(stored.hw_stage), int(stored.sw_stage), int(stored.has_fetch_shader),
                 stored.flattened_ud_buf.size());
    Shader::Gcn::SetFetchShaderOverride(&fetch);
    Shader::Optimization::SetFlatBufOverride(&stored.flattened_ud_buf);
    Shader::Pools pools;
    auto runtime_info = spec.runtime_info;
    auto binding = spec.start;
    const auto program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
    Shader::Gcn::SetFetchShaderOverride(nullptr);
    Shader::Optimization::SetFlatBufOverride(nullptr);

    if (argc == 5) {
        std::ofstream(argv[4], std::ios::binary)
            .write(reinterpret_cast<const char*>(spv.data()), spv.size() * sizeof(u32));
        return 0;
    }
    const auto cached = ReadFile(cache / (std::string{hash} + "_" + std::to_string(perm_idx) + ".spv"));
    const bool same = cached.size() == spv.size() * sizeof(u32) &&
                      std::memcmp(cached.data(), spv.data(), cached.size()) == 0;
    std::cout << hash << '_' << perm_idx << (same ? " same" : " differs") << " (" << spv.size() * 4
              << " vs " << cached.size() << " bytes)\n";
    return same ? 0 : 1;
}
