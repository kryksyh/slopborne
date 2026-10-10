// SPDX-License-Identifier: GPL-2.0-or-later
// Offline compiler probe for captured StandardParticle GS; never runs game code
// or Vulkan.
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/regs.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static std::vector<u32> ReadWords(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto size = input ? std::streamoff(input.tellg()) : -1;
  if (size <= 0 || size % 4 || size > 16 * 1024 * 1024)
    throw std::runtime_error("Invalid DWORD file: " + path.string());
  std::vector<u32> data(size_t(size) / 4);
  input.seekg(0);
  if (!input.read(reinterpret_cast<char *>(data.data()), size))
    throw std::runtime_error("Could not read " + path.string());
  return data;
}

int main(int argc, char **argv) try {
  if (argc < 3 || argc > 4 ||
      (argc == 4 && std::string_view(argv[3]) != "--compute" &&
       std::string_view(argv[3]) != "--replay" &&
       std::string_view(argv[3]) != "--export")) {
    std::cerr << "Usage: geometry-probe StandardParticle-snapshot "
                 "new-output.spv [--compute|--replay|--export]\n";
    return 2;
  }
  const std::filesystem::path root(argv[1]), output(argv[2]);
  if (std::filesystem::exists(output))
    throw std::runtime_error("Output already exists");
  auto code = ReadWords(root / "gs.bin");
  auto copy = ReadWords(root / "copy.bin");
  auto words = ReadWords(root / "regs.bin");
  auto ud_words = ReadWords(root / "gs.userdata.bin");
  if (words.size() * 4 != sizeof(AmdGpu::Regs) || ud_words.size() != 16)
    throw std::runtime_error("Snapshot ABI mismatch");
  AmdGpu::Regs regs;
  std::memcpy(&regs, words.data(), sizeof(regs));
  // This captured shader reads only ES/GS ring descriptors from the global
  // table. RingAccessElimination removes those before binding discovery. Never
  // dereference addresses from a dead game process; this is compilation, not
  // resource validation.
  if (code.size() != 1494 || code[0] != 0xbeeb03ff || copy.size() != 90 ||
      regs.vgt_gs_max_vert_out != 8 ||
      regs.primitive_type != AmdGpu::PrimitiveType::PointList)
    throw std::runtime_error(
        "Probe requires the captured StandardParticle point shader");
  const bool export_stage =
      argc == 4 && std::string_view(argv[3]) == "--export";
  if (export_stage) {
    code = ReadWords(root / "es.bin");
    ud_words = ReadWords(root / "es.userdata.bin");
    if (code.size() != 113 || ud_words.size() != 16)
      throw std::runtime_error("Unexpected StandardParticle ES snapshot");
  }
  std::array<u32, 32> dummy_ring_table{};
  const u64 table = reinterpret_cast<u64>(dummy_ring_table.data());
  std::memcpy(ud_words.data(), &table, sizeof(table));
  std::array<u32, 16> ud;
  std::copy(ud_words.begin(), ud_words.end(), ud.begin());
  using namespace Shader;
  ShaderParams params{ud, code, export_stage ? 0x16e3fa9fu : 0x8e3f8dc4u};
  Info info(export_stage ? HwStage::Export : HwStage::Geometry,
            export_stage ? SwStage::Vertex : SwStage::Geometry, params);
  RuntimeInfo runtime{};
  runtime.Initialize(info.hw_stage, info.sw_stage);
  const auto &settings =
      export_stage ? regs.es_program.settings : regs.gs_program.settings;
  runtime.props.num_user_data = settings.num_user_regs;
  runtime.props.num_input_vgprs = settings.vgpr_comp_cnt;
  runtime.props.num_allocated_vgprs =
      export_stage ? regs.es_program.NumVgprs() : regs.gs_program.NumVgprs();
  runtime.props.fp_denorm_mode32 = settings.fp_denorm_mode32;
  runtime.props.fp_denorm_mode16_64 = settings.fp_denorm_mode64;
  runtime.props.fp_round_mode32 = settings.fp_round_mode32;
  runtime.props.fp_round_mode16_64 = settings.fp_round_mode64;
  auto &gs = runtime.hw.gs;
  gs.output_vertices = regs.vgt_gs_max_vert_out;
  gs.num_invocations =
      regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
  gs.in_primitive = regs.primitive_type;
  for (u32 stream = 0; stream < GsMaxOutputStreams; ++stream)
    gs.out_primitive[stream] =
        regs.vgt_gs_out_prim_type.GetPrimitiveType(stream);
  gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
  gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
  gs.mode = regs.vgt_gs_mode.mode;
  gs.vs_copy = copy;
  gs.vs_copy_hash = params.hash;
  if (export_stage) {
    runtime.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
    runtime.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
    runtime.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
  }
  Profile profile{};
  profile.supported_spirv = 0x00010600;
  profile.subgroup_size = 32;
  profile.max_viewport_width = profile.max_viewport_height = 16384;
  profile.support_int64 = true;
  profile.support_float16 = true;
  profile.support_fp32_signed_zero_inf_nan_preserve = true;
  profile.emulate_robustness = true;
  profile.needs_buffer_offsets = true;
  profile.metal_inline_arithmetic = true;
  Pools pools;
  auto program = TranslateProgram(code, pools, info, runtime, profile);
  std::cout << "Translated " << program.ins_list.size() << " instructions, "
            << program.blocks.size() << " IR blocks, " << info.buffers.size()
            << " buffers, " << info.images.size() << " images, "
            << info.gs_copy_data.num_comps << " output components\n";
  Backend::Bindings bindings{};
  Backend::SPIRV::GeometryComputeLayout layout;
  auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings,
                                       argc == 4 ? &layout : nullptr);
  if (argc == 4 && std::string_view(argv[3]) == "--replay")
    spv = Backend::SPIRV::EmitGeometryReplayVertex(profile, layout,
                                                   gs.output_vertices);
  if (argc == 4)
    std::cout << "Compute bindings input=" << layout.input_binding
              << " output=" << layout.output_binding
              << ", attributes=" << layout.attribute_dwords
              << ", vertex stride=" << layout.vertex_stride_dwords
              << ", primitive stride=" << layout.primitive_stride_dwords
              << '\n';
  std::ofstream out(output, std::ios::binary);
  out.write(reinterpret_cast<const char *>(spv.data()), spv.size() * 4);
  if (!out)
    throw std::runtime_error("Could not write output");
  std::cout << "Wrote " << spv.size() * 4
            << " bytes of SPIR-V (not submitted)\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
