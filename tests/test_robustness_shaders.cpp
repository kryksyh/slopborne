// SPDX-License-Identifier: GPL-2.0-or-later
// Production shader backend: GPU readback with hardware robustness disabled.
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <vulkan/vulkan.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/specialization.h"
#include "vulkan_readback.h"
static std::vector<u32> shader(bool images, bool widths = false, bool shared = false, bool dma = false,
                               bool readonly = false, int arithmetic = 0) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    for (int i = 0; i < 3; ++i) {
        info.buffers.push_back(
            {.used_types = IR::Type::U32,
             .buffer_type = i == 2 ? BufferType::ResourcePresence : BufferType::Guest,
             .is_written = i != 2});
    }
    if (dma) {
        info.buffers[0].buffer_type = BufferType::BdaPagetable;
        info.buffers[0].used_types = IR::Type::U64;
        info.buffers[2].buffer_type = BufferType::FaultBuffer;
        info.buffers[2].is_written = true;
        info.buffers.push_back({.used_types = IR::Type::U32,
                                .buffer_type = BufferType::ResourcePresence});
        info.uses_dma = true;
        info.readconst_types = Info::ReadConstType::Dynamic;
    }
    if (widths)
        info.buffers[0].used_types |= IR::Type::U8 | IR::Type::U16 | IR::Type::U64 | IR::Type::F32;
    if (readonly || arithmetic)
        info.buffers[0].is_written = false;
    if (arithmetic)
        info.buffers[0].used_types |= IR::Type::F32;
    if (images) {
        info.images.push_back({.is_written = true});
        info.has_storage_images = true;
    }
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.cs.workgroup_size = {1, 1, 1};
    if (shared) {
        runtime.hw.cs.workgroup_size = {64, 1, 1};
        runtime.hw.cs.shared_memory_size = 256;
        info.shared_types = IR::Type::U32;
        info.loads.Set(IR::Attribute::LocalInvocationIndex, 0);
    }
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    const auto src = ir.Imm32(0u), dst = ir.Imm32(1u);
    auto save = [&](u32 slot, const IR::Value &value) {
        ir.StoreBufferU32(1, dst, ir.Imm32(slot), value, {});
    };
    if (dma) {
        const u64 addresses[] = {0, 65536, 0x1000000000000ULL, 0xffffffffffffffffULL};
        for (u32 i = 0; i < 4; ++i) {
            const auto address = ir.CompositeConstruct(ir.Imm32(u32(addresses[i])),
                                                      ir.Imm32(u32(addresses[i] >> 32)));
            save(i, ir.ReadConst(address, ir.Imm32(0u)));
        }
    } else if (shared) {
        const auto lane = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const auto peer = ir.BitwiseXor(lane, ir.Imm32(32u));
        ir.WriteShared(32, ir.IAdd(lane, ir.Imm32(1u)), ir.ShiftLeftLogical(lane, ir.Imm32(2u)));
        const auto value = ir.LoadShared(32, false, ir.ShiftLeftLogical(peer, ir.Imm32(2u)));
        ir.StoreBufferU32(1, dst, lane, value, {});
    } else if (widths) {
        save(0, ir.UConvert(32, ir.LoadBufferU8(src, ir.Imm32(8u), {})));
        save(1, ir.UConvert(32, ir.LoadBufferU8(src, ir.Imm32(12u), {})));
        save(2, ir.UConvert(32, ir.LoadBufferU16(src, ir.Imm32(4u), {})));
        save(3, ir.UConvert(32, ir.LoadBufferU16(src, ir.Imm32(6u), {})));
        save(4, ir.CompositeExtract(ir.UnpackUint2x32(ir.LoadBufferU64(src, ir.Imm32(0u), {})), 0));
        save(5, ir.CompositeExtract(ir.UnpackUint2x32(ir.LoadBufferU64(src, ir.Imm32(1u), {})), 0));
        save(6, ir.BitCast<IR::U32>(IR::F32{ir.LoadBufferF32(1, src, ir.Imm32(2u), {})}));
        save(7, ir.BitCast<IR::U32>(IR::F32{ir.LoadBufferF32(1, src, ir.Imm32(3u), {})}));
        ir.StoreBufferU8(src, ir.Imm32(12u), ir.Imm8(255), {});
        ir.StoreBufferU16(src, ir.Imm32(6u), ir.Imm16(65535), {});
        ir.StoreBufferU64(src, ir.Imm32(1u), ir.Imm64(0xffffffffffffffffULL), {});
        ir.StoreBufferF32(1, src, ir.Imm32(3u), ir.Imm32(123.0f), {});
    } else if (arithmetic) {
        const IR::F32 a{ir.LoadBufferF32(1, src, ir.Imm32(0u), {})};
        const IR::F32 b{ir.LoadBufferF32(1, src, ir.Imm32(1u), {})};
        const IR::F32 c{ir.LoadBufferF32(1, src, ir.Imm32(2u), {})};
        save(0, ir.BitCast<IR::U32>(IR::F32{ir.FPAdd(a, b)}));
        save(1, ir.BitCast<IR::U32>(IR::F32{ir.FPMul(a, b)}));
        save(2, ir.BitCast<IR::U32>(IR::F32{ir.FPSub(a, b)}));
        save(3, ir.BitCast<IR::U32>(IR::F32{ir.FPAdd(ir.FPMul(a, b), c)}));
        save(4, ir.BitCast<IR::U32>(IR::F32{ir.FPFma(a, b, c)}));
    } else if (readonly) {
        save(0, ir.LoadBufferU32(1, src, ir.Imm32(2u), {}));
        save(1, ir.ReadConstBuffer(src, ir.Imm32(3u), {}));
        ir.StoreBufferU32(4, dst, ir.Imm32(2u),
                         ir.LoadBufferU32(4, src, ir.Imm32(2u), {}), {});
        save(6, ir.LoadBufferU32(1, src, ir.Imm32(0xffffffffu), {}));
        save(7, ir.ReadConstBuffer(src, ir.Imm32(0u), {}));
    } else if (!images) {
        save(0, ir.LoadBufferU32(1, src, ir.Imm32(2u), {}));
        save(1, ir.LoadBufferU32(1, src, ir.Imm32(3u), {}));
        save(2, ir.ReadConstBuffer(src, ir.Imm32(3u), {}));
        save(3, ir.BufferAtomicIAdd(src, ir.Imm32(0u), ir.Imm32(5u), {}));
        save(4, ir.BufferAtomicIAdd(src, ir.Imm32(3u), ir.Imm32(5u), {}));
        const auto vector = ir.LoadBufferU32(4, src, ir.Imm32(2u), {});
        ir.StoreBufferU32(4, dst, ir.Imm32(5u), vector, {});
        save(9, ir.LoadBufferU32(1, src, ir.Imm32(0xffffffffu), {}));
        ir.StoreBufferU32(
            4, src, ir.Imm32(2u),
            ir.CompositeConstruct(ir.Imm32(7u), ir.Imm32(8u), ir.Imm32(9u), ir.Imm32(10u)), {});
        ir.StoreBufferU32(1, src, ir.Imm32(0xffffffffu), ir.Imm32(99u), {});
        save(10, ir.LoadBufferU32(1, src, ir.Imm32(0u), {}));
    } else {
        const auto xy = ir.CompositeConstruct(ir.Imm32(0u), ir.Imm32(0u));
        const auto color =
            ir.CompositeConstruct(ir.Imm32(1.0f), ir.Imm32(2.0f), ir.Imm32(3.0f), ir.Imm32(4.0f));
        ir.ImageWrite(src, xy, {}, {}, color, {});
        const auto read = ir.ImageRead(src, xy, {}, {}, {});
        for (u32 i = 0; i < 4; ++i)
            save(i, ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(read, i)}));
        const auto dims = ir.ImageQueryDimension(src, ir.Imm32(0u), ir.Imm1(false), {});
        ir.StoreBufferU32(4, dst, ir.Imm32(4u), dims, {});
    }
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.emulate_robustness = true;
    profile.bounded_buffer_reads = true;
    profile.metal_inline_arithmetic = arithmetic == 2;
    profile.support_fp32_signed_zero_inf_nan_preserve = arithmetic != 0;
    profile.sparse_page_shift = 16;
    profile.needs_buffer_offsets = true;
    profile.support_int8 = profile.support_int16 = profile.support_int64 = true;
    if (shared) {
        profile.subgroup_size = 32;
        profile.needs_lds_barriers = true;
        Optimization::SharedMemoryBarrierPass(program, runtime, profile);
    }
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

static void expect(u32 actual, u32 expected, const char *label, u32 index) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s[%u]: %u != %u\n", label, index, actual, expected);
        std::exit(1);
    }
}
static int geometry_test(const char* dump_path, bool compile_only = false) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Geometry;
    info.sw_stage = SwStage::Geometry;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.gs.in_primitive = AmdGpu::PrimitiveType::PointList;
    runtime.hw.gs.num_invocations = 1;
    runtime.hw.gs.output_vertices = 3;
    runtime.hw.gs.in_vertex_data_size = 8;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    for (u32 comp = 0; comp < 4; ++comp) {
        info.stores.Set(IR::Attribute::Position0, comp);
        info.stores.Set(IR::Attribute::Param0, comp);
        ir.SetAttribute(IR::Attribute::Position0, ir.GetAttribute(IR::Attribute::Position0, comp, 0), comp);
        ir.SetAttribute(IR::Attribute::Param0, ir.GetAttribute(IR::Attribute::Param0, comp, 0), comp);
    }
    info.loads.Set(IR::Attribute::PrimitiveId, 0);
    ir.SetAttribute(IR::Attribute::Param0,
                    ir.BitCast<IR::F32>(ir.GetAttributeU32(IR::Attribute::PrimitiveId)), 3);
    const auto original_x = ir.GetAttribute(IR::Attribute::Position0, 0, 0);
    for (u32 vertex = 0; vertex < 4; ++vertex) {
        ir.SetAttribute(IR::Attribute::Position0, ir.FPAdd(original_x, ir.Imm32(float(vertex))), 0);
        ir.EmitVertex(); // Fourth emit intentionally exceeds max_vertices.
        if (vertex == 1) ir.EmitPrimitive();
    }
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    Backend::Bindings bindings{};
    Backend::SPIRV::GeometryComputeLayout layout;
    const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings, &layout);
    expect(layout.input_binding, 0, "geometry input binding", 0);
    expect(layout.output_binding, 1, "geometry output binding", 0);
    expect(layout.primitive_stride_dwords, 29, "geometry record size", 0);
    if (dump_path) {
        if (std::filesystem::exists(dump_path)) return 2;
        std::ofstream file(dump_path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
    }
    if (compile_only) return 0;
    Gpu gpu(code, false);
    struct Case {u32 count, input_points, output_points, expected; bool short_header;};
    const Case cases[] = {{3,4,3,3,false}, {0,4,3,0,false}, {100,2,3,2,false},
                          {3,4,2,2,false}, {3,4,0,0,false}, {3,4,3,0,true}};
    for (u32 test = 0; test < std::size(cases); ++test) {
        const auto& c = cases[test];
        std::vector<u32> input(c.short_header ? 2 : 4 + c.input_points * 8);
        input[0] = c.count;
        if (input.size() >= 4) input[1] = test == 0 ? 2 : 0;
        if (!c.short_header)
            for (u32 p = 0; p < c.input_points; ++p)
                for (u32 component = 0; component < 8; ++component)
                    input[4 + p * 8 + component] = std::bit_cast<u32>(float(10 * p + component));
        gpu.run(true, 0, nullptr, input, std::max(4u, c.output_points * 29 * 4));
        for (u32 p = 0; p < c.expected; ++p) {
            const u32 base = p * 29;
            expect(gpu.mapped[1][base], 3, "geometry vertex count", test);
            expect(gpu.mapped[1][base + 1], 1, "geometry overflow flag", test);
            for (u32 vertex = 0; vertex < 3; ++vertex) {
                const u32 record = base + 2 + vertex * 9;
                expect(gpu.mapped[1][record], vertex < 2 ? 0 : 2, "geometry strip start", test);
                for (u32 component = 0; component < 8; ++component) {
                    const float value = float(10 * p + component + (component == 0 ? vertex : 0));
                    expect(gpu.mapped[1][record + 1 + component],
                            component == 7 ? (test == 0 ? p % 2 : p) : std::bit_cast<u32>(value),
                            "geometry attribute", test);
                }
            }
        }
        for (u32 word = c.expected * 29; word < 1024; ++word) {
            const bool inactive_header = word / 29 < c.output_points && word % 29 < 2;
            expect(gpu.mapped[1][word], inactive_header ? 0u : 0xdeadbeefu,
                    "geometry retired header/unwritten guard", word);
        }
    }
    std::puts("PASS: point-GS compute attributes, cuts, overflow and input/output bounds (6 cases)");
    return 0;
}

static int export_test(const char* dump_path, bool compile_only = false) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Export;
    info.sw_stage = SwStage::Vertex;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.es.vertex_data_size = 8;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.Prologue();
    const IR::Attribute builtins[] = {IR::Attribute::VertexId, IR::Attribute::InstanceId,
                                     IR::Attribute::BaseVertex, IR::Attribute::BaseInstance};
    for (u32 comp = 0; comp < 4; ++comp) {
        info.loads.Set(builtins[comp], 0);
        info.stores.Set(IR::Attribute::Position0, comp);
        ir.SetAttribute(IR::Attribute::Position0,
                        ir.BitCast<IR::F32>(ir.GetAttributeU32(builtins[comp])), comp);
    }
    info.stores.Set(IR::Attribute::Param0, 0);
    info.stores.Set(IR::Attribute::Param0, 3);
    ir.SetAttribute(IR::Attribute::Param0, ir.Imm32(42.0f), 0);
    ir.SetAttribute(IR::Attribute::Param0,
                    ir.BitCast<IR::F32>(IR::U32{ir.IAdd(ir.GetAttributeU32(IR::Attribute::VertexId),
                                              ir.GetAttributeU32(IR::Attribute::InstanceId))}), 3);
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    Backend::Bindings bindings{};
    Backend::SPIRV::GeometryComputeLayout layout;
    const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings, &layout);
    expect(layout.input_binding, 0, "export input binding", 0);
    expect(layout.output_binding, 1, "export output binding", 0);
    expect(layout.attribute_dwords, 8, "export stride", 0);
    if (dump_path) {
        if (std::filesystem::exists(dump_path)) return 2;
        std::ofstream file(dump_path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
    }
    if (compile_only) return 0;
    Gpu gpu(code, false);
    struct Case { u32 vertices, instances, base_vertex, base_instance, indexed, input_words, output_words; };
    const Case cases[] = {
        {3,2,5,7,0,8,60}, {4,3,0xfffffffeu,10,1,12,100},
        {4,2,0,0,1,11,68}, // incomplete index list must reject the entire draw
        {0,5,0,0,0,8,20}, {3,0,0,0,0,8,20},
        {3,2,0,0,0,7,60}, {3,2,0,0,0,8,3},
        {3,2,0,0,0,8,4}, {3,2,0,0,0,8,23},
        {0xffffffffu,0xffffffffu,0,0,0,8,44},
        {3,0xffffffffu,0,0xfffffffeu,0,8,60},
        {3,2,0,0,2,12,60}, {3,2,0,0,0,1,60},
        {4,2,0xffffffffu,0xffffffffu,1,12,68},
    };
    for (u32 test = 0; test < std::size(cases); ++test) {
        const auto& c = cases[test];
        std::vector<u32> input(c.input_words);
        const u32 header[] = {c.vertices,c.instances,c.base_vertex,c.base_instance,c.indexed,0,0,0};
        std::copy_n(header, std::min(8u,c.input_words), input.begin());
        const u32 indices[] = {7,2,7,0xffffffffu};
        for (u32 i = 8; i < c.input_words; ++i) input[i] = indices[i-8];
        std::array<u32,1024> expected;
        expected.fill(0xdeadbeefu);
        if (c.output_words >= 4) {
            std::fill_n(expected.begin(), 4, 0u);
            const bool valid = c.input_words >= 8 && c.indexed <= 1 &&
                               (!c.indexed || c.vertices <= c.input_words - 8);
            const u32 count = valid ? u32(std::min<u64>(u64(c.vertices) * c.instances,
                                                       (c.output_words - 4) / 8)) : 0;
            expect(count <= 64, 1, "export test dispatch capacity", test);
            expected[0] = count;
            expected[1] = valid && c.vertices != 0 ? c.vertices : 0;
            for (u32 point = 0; point < count; ++point) {
                const u32 vertex = (c.indexed ? indices[point % c.vertices] : point % c.vertices) + c.base_vertex;
                const u32 instance = point / c.vertices + c.base_instance;
                const u32 values[] = {vertex,instance,c.base_vertex,c.base_instance,
                                      std::bit_cast<u32>(42.0f),0,0,vertex+instance};
                std::copy_n(values,8,expected.begin()+4+point*8);
            }
        }
        gpu.run(true,0,nullptr,input,c.output_words*4);
        for (u32 word = 0; word < 1024; ++word)
            expect(gpu.mapped[1][word],expected[word],"export readback/guard",word);
    }
    std::puts("PASS: ES compute indexed/nonindexed vertices, instances, base offsets, overflow and bounds (14 cases)");
    return 0;
}

static int specialization_test() {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    info.images.push_back({.is_array = true});
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    Profile profile{};
    profile.emulate_robustness = true;
    const auto make = [&](AmdGpu::ImageType type, bool bound, bool integer = false) {
        auto sharp = AmdGpu::Image::Null(false);
        sharp.type = u64(type);
        sharp.base_address = bound ? 0x1234 : 0;
        if (integer) sharp.num_format = u64(AmdGpu::NumberFormat::Uint);
        std::memcpy(info.images[0].sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
        return StageSpecialization(info, runtime, profile, {});
    };
    const auto array = make(AmdGpu::ImageType::Color2DArray, true);
    const auto absent = make(AmdGpu::ImageType::Color2D, false);
    if (array == absent || absent == array) {
        std::fprintf(stderr, "Incompatible missing 2D texture reused a 2D-array module\n");
        return 1;
    }
    const auto absent_array = make(AmdGpu::ImageType::Color2DArray, false);
    if (absent_array.images[0].type != AmdGpu::ImageType::Color2DArray ||
        absent_array.images[0].num_bindings != 1 || absent_array.bitset.any() ||
        !(array == absent_array)) {
        std::fprintf(stderr, "Missing typed array lost its interface or compatible reuse\n");
        return 1;
    }
    if (absent == absent_array || absent_array == absent) {
        std::fprintf(stderr, "All-null shortcut ignored image dimensions\n");
        return 1;
    }
    const auto planar = make(AmdGpu::ImageType::Color2D, true);
    const auto integer = make(AmdGpu::ImageType::Color2D, true, true);
    if (!(planar == absent) || integer == absent || absent == integer) {
        std::fprintf(stderr, "Null matching did not preserve sampled scalar type\n");
        return 1;
    }
    auto shifted = absent;
    ++shifted.start.unified;
    if (shifted == absent || absent == shifted) {
        std::fprintf(stderr, "Missing descriptors ignored their binding locations\n");
        return 1;
    }
    std::puts("PASS: null texture dimensionality, scalar type, compatible reuse and bindings");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--specialization-only")
        return specialization_test();
    if (argc >= 2 && std::string_view(argv[1]) == "--export-only")
        return export_test(argc > 2 ? argv[2] : nullptr);
    if (argc >= 2 && std::string_view(argv[1]) == "--export-compile-only")
        return export_test(argc > 2 ? argv[2] : nullptr, true);
    if (argc >= 2 && std::string_view(argv[1]) == "--geometry-only")
        return geometry_test(argc > 2 ? argv[2] : nullptr);
    if (argc >= 2 && std::string_view(argv[1]) == "--geometry-compile-only")
        return geometry_test(argc > 2 ? argv[2] : nullptr, true);
    setenv("BB_DIRECT_MEMORY_ACCESS", "1", 1);
    {
        Shader::Info info{};
        std::array<u32, 16> user_data{};
        info.user_data = user_data;
        Shader::Gcn::VertexAttribute attr{};
        attr.dword_offset = 8;
        attr.inst_offset = 16;
        const auto missing = attr.GetSharp(info);
        if (missing) {
            std::fprintf(stderr, "Null vertex table became a bound descriptor\n");
            return 1;
        }
        std::array<AmdGpu::Buffer, 3> table{};
        table[2].base_address = 0x123400;
        table[2].num_records = 4;
        const auto pointer = table.data();
        std::memcpy(user_data.data(), &pointer, sizeof(pointer));
        if (attr.GetSharp(info).base_address != 0x123410) {
            std::fprintf(stderr, "Valid vertex descriptor offset was not preserved\n");
            return 1;
        }
        std::puts("PASS: null vertex tables and valid descriptor offsets");
    }
    const auto dir = argc > 1 ? argv[1] : "out/macos/robustness";
    std::filesystem::create_directories(dir);
    std::vector<std::array<u32, 5>> arithmetic_reference;
    for (int mode = 0; mode < 8; ++mode) {
        const bool images = mode == 1, widths = mode == 2, shared = mode == 3, dma = mode == 4;
        const bool readonly = mode == 5;
        const int arithmetic = mode >= 6 ? mode - 5 : 0;
        const auto code = shader(images, widths, shared, dma, readonly, arithmetic);
        std::ofstream out(std::filesystem::path(dir) / (images   ? "images.spv"
                                                        : arithmetic == 1 ? "arithmetic-original.spv"
                                                        : arithmetic == 2 ? "arithmetic-inline.spv"
                                                        : readonly ? "readonly.spv"
                                                        : dma ? "dma.spv"
                                                        : shared ? "shared.spv"
                                                        : widths ? "widths.spv"
                                                                 : "buffers.spv"),
                          std::ios::binary);
        out.write(reinterpret_cast<const char *>(code.data()), code.size() * 4);
        out.close();
        if (argc > 2 && std::strcmp(argv[2], "--emit-only") == 0)
            continue;
        Gpu gpu(code, images, dma);
        if (arithmetic) {
            const u32 edge[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
                                0x3f800000, 0xbf800000, 0x3f800001, 0x3f800002,
                                0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000};
            std::vector<std::array<u32, 4>> cases;
            for (u32 a : edge) for (u32 b : edge)
                cases.push_back({a, b, 0xbf800002, 0});
            // Multiplication rounds before addition in the first expression,
            // while the explicit fused operation retains its tiny remainder.
            cases.push_back({0x3f800001, 0x3f800001, 0xbf800002, 0});
            u32 seed = 0x31415926;
            for (unsigned i = 0; i < 256; ++i) {
                std::array<u32, 4> values{};
                for (auto& value : values) {
                    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                    value = seed;
                }
                cases.push_back(values);
            }
            for (size_t i = 0; i < cases.size(); ++i) {
                gpu.run(true, 0, cases[i].data());
                std::array<u32, 5> actual{};
                std::copy_n(gpu.mapped[1], actual.size(), actual.begin());
                if (arithmetic == 1) {
                    arithmetic_reference.push_back(actual);
                } else {
                    for (size_t j = 0; j < actual.size(); ++j) {
                        const u32 expected = arithmetic_reference[i][j];
                        // NaN payload propagation is not specified by SPIR-V.
                        if (std::isnan(std::bit_cast<float>(expected)) &&
                            std::isnan(std::bit_cast<float>(actual[j]))) continue;
                        expect(actual[j], expected, "Metal inline arithmetic equivalence", i * 5 + j);
                    }
                }
                if (i == 225) {
                    expect(actual[3], 0, "separate multiplication rounding", 3);
                    expect(actual[4], 0x28800000, "explicit fused rounding", 4);
                }
            }
            std::printf("PASS: %s arithmetic, %zu cases including signed zero, NaN, infinity, denormals and rounding\n",
                        arithmetic == 1 ? "reference" : "inline", cases.size());
            continue;
        }
        if (dma) {
            gpu.run(true);
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[1][i], 0, "DMA missing/invalid page", i);
            expect(gpu.mapped[2][0], 1, "DMA valid missing page fault", 0);
            for (u32 i = 1; i < 1024; ++i)
                expect(gpu.mapped[2][i], 0, "DMA fault bounds", i);
            std::puts("PASS: DMA page bounds, 64-bit page overflow and missing-page faults");
            continue;
        }
        if (shared) {
            for (int repeat = 0; repeat < 16; ++repeat) {
                gpu.run(true);
                for (u32 lane = 0; lane < 64; ++lane)
                    expect(gpu.mapped[1][lane], (lane ^ 32u) + 1, "wave64 shared exchange", lane);
            }
            std::puts("PASS: 64-lane shared memory exchange across 32-lane subgroups");
            continue;
        }
        gpu.run(false);
        for (u32 i = 0; i < 11; ++i)
            expect(gpu.mapped[1][i], 0, "null result", i);
        for (u32 i = 0; i < 4; ++i)
            expect(gpu.mapped[0][i], 11 * (i + 1), "null backing", i);
        if (images) {
            const float untouched = 9;
            u32 bits;
            std::memcpy(&bits, &untouched, 4);
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[1][128 + i], bits, "null image write isolation", i);
        }
        gpu.run(true);
        if (readonly) {
            const u32 expected[] = {33, 0, 33, 0, 0, 0, 0, 11};
            for (u32 i = 0; i < 8; ++i)
                expect(gpu.mapped[1][i], expected[i], "readonly bounds result", i);
            gpu.run(true, 4);
            expect(gpu.mapped[1][6], 0, "readonly address overflow", 6);
            expect(gpu.mapped[1][7], 22, "readonly shifted constant buffer", 7);
            std::puts("PASS: readonly redirected bounds, partial vectors, null bindings and overflow");
            continue;
        } else if (widths) {
            const u32 expected[] = {33, 0, 33, 0, 11, 0, 33, 0};
            for (u32 i = 0; i < 8; ++i)
                expect(gpu.mapped[1][i], expected[i], "typed buffer result", i);
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[0][i], 11 * (i + 1), "typed discarded writes", i);
        } else if (!images) {
            const u32 expected[] = {33, 0, 0, 11, 0, 33, 0, 0, 0, 0, 16};
            for (u32 i = 0; i < 11; ++i)
                expect(gpu.mapped[1][i], expected[i], "buffer result", i);
            const u32 backing[] = {16, 22, 7, 44};
            for (u32 i = 0; i < 4; ++i)
                expect(gpu.mapped[0][i], backing[i], "buffer backing", i);
            gpu.run(true, 4);
            expect(gpu.mapped[1][9], 0, "address overflow read", 0);
            expect(gpu.mapped[0][0], 11, "address overflow write", 0);
        } else {
            const float expected[] = {1, 2, 3, 4};
            for (u32 i = 0; i < 4; ++i) {
                u32 bits;
                std::memcpy(&bits, &expected[i], 4);
                expect(gpu.mapped[1][i], bits, "image result", i);
            }
        }
        std::printf(
            "PASS: %s\n",
            images ? "null image reads/writes/queries and valid image access"
            : widths
                ? "8/16/64-bit and float buffer bounds and null access"
                : "buffer bounds, partial vectors, atomics, null bindings and address overflow");
    }
}
