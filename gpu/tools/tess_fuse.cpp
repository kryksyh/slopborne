// SPDX-License-Identifier: GPL-2.0-or-later
// Offline check of the quad tessellation fusion: links cached local-stage and domain-stage
// SPIR-V modules into one vertex shader, with the hull stage's tessellation pattern.
// Usage: tess-fuse <local.spv> <hull.spv> <domain.spv> <out.spv>   (exit 1: not fusable)
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

#include "video_core/renderer_vulkan/vk_tess_fusion.h"

static std::vector<u32> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<u32> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: tess-fuse <local.spv> <hull.spv> <domain.spv> <out.spv>\n";
        return 2;
    }
    const auto fused = Vulkan::FuseQuadTessellation(Read(argv[1]), Read(argv[2]), Read(argv[3]));
    if (!fused) {
        std::cerr << "not fusable\n";
        return 1;
    }
    std::ofstream(argv[4], std::ios::binary)
        .write(reinterpret_cast<const char*>(fused->spirv.data()), fused->spirv.size() * 4);
    std::cout << fused->vertices << " vertices per patch\n";
    return 0;
}
