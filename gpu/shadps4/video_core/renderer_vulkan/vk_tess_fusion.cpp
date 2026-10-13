// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string_view>
#include <spirv/unified1/spirv.hpp11>

#include "video_core/renderer_vulkan/vk_tess_fusion.h"

namespace Vulkan {
namespace {

using Op = spv::Op;

/// BB_TESS_FUSE_LOG=1: why a pipeline was not fused.
#define FUSE_FAIL()                                                                                \
    do {                                                                                           \
        if (std::getenv("BB_TESS_FUSE_LOG")) {                                                     \
            std::printf("Tessellation fusion: refused at %s:%d\n", __FILE__, __LINE__);           \
        }                                                                                          \
        return std::nullopt;                                                                       \
    } while (false)

struct Inst {
    Op op;
    std::vector<u32> words; ///< operands, without the opcode word
};

struct Module {
    u32 version{};
    u32 bound{};
    std::vector<Inst> insts;
};

std::optional<Module> Parse(std::span<const u32> code) {
    if (code.size() < 5 || code[0] != spv::MagicNumber) {
        return std::nullopt;
    }
    Module module{.version = code[1], .bound = code[3]};
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        if (count == 0 || i + count > code.size()) {
            return std::nullopt;
        }
        module.insts.push_back(
            {Op(code[i] & 0xffff), {code.begin() + i + 1, code.begin() + i + count}});
        i += count;
    }
    return module;
}

/// Calls f on every operand that is an id. False for an instruction this does not know.
template <typename F>
bool ForEachId(Inst& inst, F&& f) {
    auto& w = inst.words;
    const auto ids = [&](size_t first, size_t last) {
        for (size_t i = first; i < std::min(last, w.size()); ++i) {
            f(w[i]);
        }
    };
    const size_t all = w.size();
    switch (inst.op) {
    // Every operand is an id.
    case Op::OpAccessChain: case Op::OpInBoundsAccessChain: case Op::OpPtrAccessChain:
    case Op::OpFunctionCall: case Op::OpCompositeConstruct: case Op::OpSelect: case Op::OpPhi:
    case Op::OpBitcast: case Op::OpConvertFToU: case Op::OpConvertFToS: case Op::OpConvertSToF:
    case Op::OpConvertUToF: case Op::OpUConvert: case Op::OpSConvert: case Op::OpFConvert:
    case Op::OpSNegate: case Op::OpFNegate: case Op::OpIAdd: case Op::OpFAdd: case Op::OpISub:
    case Op::OpFSub: case Op::OpIMul: case Op::OpFMul: case Op::OpUDiv: case Op::OpSDiv:
    case Op::OpFDiv: case Op::OpUMod: case Op::OpSRem: case Op::OpSMod: case Op::OpFRem:
    case Op::OpFMod: case Op::OpVectorTimesScalar: case Op::OpDot: case Op::OpIAddCarry:
    case Op::OpISubBorrow: case Op::OpUMulExtended: case Op::OpSMulExtended:
    case Op::OpShiftRightLogical: case Op::OpShiftRightArithmetic: case Op::OpShiftLeftLogical:
    case Op::OpBitwiseOr: case Op::OpBitwiseXor: case Op::OpBitwiseAnd: case Op::OpNot:
    case Op::OpBitFieldInsert: case Op::OpBitFieldSExtract: case Op::OpBitFieldUExtract:
    case Op::OpBitReverse: case Op::OpBitCount: case Op::OpAny: case Op::OpAll: case Op::OpIsNan:
    case Op::OpIsInf: case Op::OpLogicalEqual: case Op::OpLogicalNotEqual: case Op::OpLogicalOr:
    case Op::OpLogicalAnd: case Op::OpLogicalNot: case Op::OpIEqual: case Op::OpINotEqual:
    case Op::OpUGreaterThan: case Op::OpSGreaterThan: case Op::OpUGreaterThanEqual:
    case Op::OpSGreaterThanEqual: case Op::OpULessThan: case Op::OpSLessThan:
    case Op::OpULessThanEqual: case Op::OpSLessThanEqual: case Op::OpFOrdEqual:
    case Op::OpFUnordEqual: case Op::OpFOrdNotEqual: case Op::OpFUnordNotEqual:
    case Op::OpFOrdLessThan: case Op::OpFUnordLessThan: case Op::OpFOrdGreaterThan:
    case Op::OpFUnordGreaterThan: case Op::OpFOrdLessThanEqual: case Op::OpFUnordLessThanEqual:
    case Op::OpFOrdGreaterThanEqual: case Op::OpFUnordGreaterThanEqual:
    case Op::OpVectorExtractDynamic: case Op::OpVectorInsertDynamic: case Op::OpCopyObject:
    case Op::OpConstantComposite: case Op::OpTypeStruct: case Op::OpTypeFunction:
    case Op::OpTypeArray: case Op::OpTypeRuntimeArray: case Op::OpTypeSampledImage:
    case Op::OpSampledImage: case Op::OpImage: case Op::OpImageQuerySize:
    case Op::OpImageQuerySizeLod: case Op::OpImageQueryLevels: case Op::OpImageQuerySamples:
    case Op::OpReturnValue: case Op::OpBranch: case Op::OpLabel: case Op::OpConstantTrue:
    case Op::OpConstantFalse: case Op::OpConstantNull: case Op::OpUndef:
    case Op::OpFunctionParameter: case Op::OpAtomicLoad: case Op::OpAtomicStore:
    case Op::OpAtomicExchange: case Op::OpAtomicCompareExchange: case Op::OpAtomicIIncrement:
    case Op::OpAtomicIDecrement: case Op::OpAtomicIAdd: case Op::OpAtomicISub:
    case Op::OpAtomicSMin: case Op::OpAtomicUMin: case Op::OpAtomicSMax: case Op::OpAtomicUMax:
    case Op::OpAtomicAnd: case Op::OpAtomicOr: case Op::OpAtomicXor: case Op::OpControlBarrier:
    case Op::OpMemoryBarrier: case Op::OpTypeVoid: case Op::OpTypeBool: case Op::OpTypeSampler:
        ids(0, all);
        return true;
    case Op::OpReturn: case Op::OpUnreachable: case Op::OpKill: case Op::OpFunctionEnd:
        return true;
    case Op::OpTypeInt: case Op::OpTypeFloat:
        ids(0, 1);
        return true;
    case Op::OpTypeVector: case Op::OpTypeMatrix: case Op::OpTypeImage:
        ids(0, 2);
        return true;
    case Op::OpTypePointer: // result, storage class, type
        ids(0, 1);
        ids(2, 3);
        return true;
    case Op::OpConstant: case Op::OpSpecConstant: case Op::OpArrayLength:
    case Op::OpCompositeExtract:
        ids(0, inst.op == Op::OpCompositeExtract || inst.op == Op::OpArrayLength ? 3 : 2);
        return true;
    case Op::OpVariable: // type, result, storage class, initializer
        ids(0, 2);
        ids(3, all);
        return true;
    case Op::OpLoad: // type, result, pointer, memory access literals
        ids(0, 3);
        return true;
    case Op::OpStore:
        ids(0, 2);
        return true;
    case Op::OpCompositeInsert: case Op::OpVectorShuffle:
        ids(0, 4);
        return true;
    case Op::OpExtInst: // type, result, set, instruction, operands
        ids(0, 3);
        ids(4, all);
        return true;
    case Op::OpFunction: // type, result, control, function type
        ids(0, 2);
        ids(3, 4);
        return true;
    case Op::OpSelectionMerge:
        ids(0, 1);
        return true;
    case Op::OpLoopMerge:
        ids(0, 2);
        return true;
    case Op::OpBranchConditional:
        ids(0, 3);
        return true;
    case Op::OpSwitch: // selector, default, (literal, label)...
        ids(0, 2);
        for (size_t i = 3; i < all; i += 2) {
            f(w[i]);
        }
        return true;
    case Op::OpImageSampleImplicitLod: case Op::OpImageSampleExplicitLod: case Op::OpImageFetch:
    case Op::OpImageGather: case Op::OpImageRead: case Op::OpImageQueryLod:
        // type, result, image, coordinate, [component | ...], operand mask, ids
        ids(0, inst.op == Op::OpImageGather ? 5 : 4);
        ids(inst.op == Op::OpImageGather ? 6 : 5, all);
        return true;
    case Op::OpImageSampleDrefImplicitLod: case Op::OpImageSampleDrefExplicitLod:
    case Op::OpImageDrefGather:
        ids(0, 5);
        ids(6, all);
        return true;
    case Op::OpImageWrite: // image, coordinate, texel, operand mask, ids
        ids(0, 3);
        ids(4, all);
        return true;
    case Op::OpDecorate: case Op::OpMemberDecorate: case Op::OpName: case Op::OpMemberName:
        ids(0, 1);
        return true;
    default:
        return false;
    }
}

bool IsTypeOrGlobal(Op op) {
    switch (op) {
    case Op::OpTypeVoid: case Op::OpTypeBool: case Op::OpTypeInt: case Op::OpTypeFloat:
    case Op::OpTypeVector: case Op::OpTypeMatrix: case Op::OpTypeImage: case Op::OpTypeSampler:
    case Op::OpTypeSampledImage: case Op::OpTypeArray: case Op::OpTypeRuntimeArray:
    case Op::OpTypeStruct: case Op::OpTypePointer: case Op::OpTypeFunction:
    case Op::OpConstant: case Op::OpConstantComposite: case Op::OpConstantTrue:
    case Op::OpConstantFalse: case Op::OpConstantNull: case Op::OpSpecConstant:
    case Op::OpUndef: case Op::OpVariable:
        return true;
    default:
        return false;
    }
}

/// Non-aggregate types must be declared once: the local stage's copies map to the domain's.
bool IsUniqueType(Op op) {
    switch (op) {
    case Op::OpTypeVoid: case Op::OpTypeBool: case Op::OpTypeInt: case Op::OpTypeFloat:
    case Op::OpTypeVector: case Op::OpTypeMatrix: case Op::OpTypeImage: case Op::OpTypeSampler:
    case Op::OpTypeSampledImage: case Op::OpTypePointer: case Op::OpTypeFunction:
        return true;
    default:
        return false;
    }
}

/// Types have their result id first, constants and variables after their type.
size_t ResultIndex(Op op) {
    switch (op) {
    case Op::OpTypeVoid: case Op::OpTypeBool: case Op::OpTypeInt: case Op::OpTypeFloat:
    case Op::OpTypeVector: case Op::OpTypeMatrix: case Op::OpTypeImage: case Op::OpTypeSampler:
    case Op::OpTypeSampledImage: case Op::OpTypeArray: case Op::OpTypeRuntimeArray:
    case Op::OpTypeStruct: case Op::OpTypePointer: case Op::OpTypeFunction:
        return 0;
    default:
        return 1;
    }
}

bool IsDebug(Op op) {
    return op == Op::OpString || op == Op::OpSource || op == Op::OpSourceExtension ||
           op == Op::OpSourceContinued || op == Op::OpName || op == Op::OpMemberName ||
           op == Op::OpModuleProcessed;
}

bool IsTessellationMode(u32 mode) {
    switch (spv::ExecutionMode(mode)) {
    case spv::ExecutionMode::Quads: case spv::ExecutionMode::Triangles:
    case spv::ExecutionMode::Isolines: case spv::ExecutionMode::SpacingEqual:
    case spv::ExecutionMode::SpacingFractionalEven: case spv::ExecutionMode::SpacingFractionalOdd:
    case spv::ExecutionMode::VertexOrderCw: case spv::ExecutionMode::VertexOrderCcw:
    case spv::ExecutionMode::PointMode: case spv::ExecutionMode::OutputVertices:
        return true;
    default:
        return false;
    }
}

std::vector<u32> TypeKey(const Inst& inst) {
    std::vector<u32> key{u32(inst.op)};
    key.insert(key.end(), inst.words.begin() + 1, inst.words.end()); // without the result id
    return key;
}

std::optional<u32> BuiltIn(const Module& m, u32 id) {
    for (const auto& inst : m.insts) {
        if (inst.op == Op::OpDecorate && inst.words.size() >= 3 && inst.words[0] == id &&
            spv::Decoration(inst.words[1]) == spv::Decoration::BuiltIn) {
            return inst.words[2];
        }
    }
    return std::nullopt;
}

const Inst* Definition(const Module& m, u32 id) {
    for (const auto& inst : m.insts) {
        if (IsTypeOrGlobal(inst.op) && inst.words.size() > ResultIndex(inst.op) &&
            inst.words[ResultIndex(inst.op)] == id) {
            return &inst;
        }
    }
    return nullptr;
}

/// The number of vec4 slots of an (array of) vec4 type: array length constant value.
std::optional<u32> ArrayLength(const Module& m, u32 array_type) {
    const Inst* array = Definition(m, array_type);
    if (!array || array->op != Op::OpTypeArray) {
        return std::nullopt;
    }
    const Inst* length = Definition(m, array->words[2]);
    if (!length || length->op != Op::OpConstant || length->words.size() < 3) {
        return std::nullopt;
    }
    return length->words[2];
}

/// The constants the hull stage stores into the tessellation levels: outer 0..3, inner 0..1.
std::optional<std::array<float, 6>> ConstantLevels(const Module& hull) {
    u32 outer = 0, inner = 0;
    std::map<u32, u32> constants; // id -> bits
    for (const auto& inst : hull.insts) {
        if (inst.op == Op::OpDecorate && inst.words.size() >= 3 &&
            spv::Decoration(inst.words[1]) == spv::Decoration::BuiltIn) {
            if (spv::BuiltIn(inst.words[2]) == spv::BuiltIn::TessLevelOuter) {
                outer = inst.words[0];
            } else if (spv::BuiltIn(inst.words[2]) == spv::BuiltIn::TessLevelInner) {
                inner = inst.words[0];
            }
        } else if (inst.op == Op::OpConstant && inst.words.size() == 3) {
            constants[inst.words[1]] = inst.words[2];
        }
    }
    if (!outer || !inner) {
        return std::nullopt;
    }
    std::map<u32, u32> pointers; // access chain -> level (outer 0..3, inner 4..5)
    std::array<std::optional<u32>, 6> stored{};
    for (const auto& inst : hull.insts) {
        if ((inst.op == Op::OpAccessChain || inst.op == Op::OpInBoundsAccessChain) &&
            inst.words.size() >= 3 && (inst.words[2] == outer || inst.words[2] == inner)) {
            const u32 first = inst.words[2] == outer ? 0 : 4, count = first == 0 ? 4 : 2;
            const auto index = inst.words.size() == 4 ? constants.find(inst.words[3])
                                                      : constants.end();
            if (index == constants.end() || index->second >= count) {
                return std::nullopt;
            }
            pointers[inst.words[1]] = first + index->second;
        } else if (inst.op == Op::OpStore) {
            if (inst.words[0] == outer || inst.words[0] == inner) {
                return std::nullopt;
            }
            const auto level = pointers.find(inst.words[0]);
            if (level == pointers.end()) {
                continue;
            }
            const auto value = constants.find(inst.words[1]);
            auto& slot = stored[level->second];
            if (value == constants.end() || (slot && *slot != value->second)) {
                return std::nullopt;
            }
            slot = value->second;
        }
    }
    std::array<float, 6> levels{};
    for (u32 i = 0; i < 6; ++i) {
        if (!stored[i]) {
            return std::nullopt;
        }
        levels[i] = std::bit_cast<float>(*stored[i]);
    }
    return levels;
}

/// Equal-spacing quad tessellation with these levels: all 1 give two triangles (`segments` 0);
/// inner levels up to 2 leave only the centre inside, so the triangles fan from it to the outer
/// edges, each cut into `segments`.
struct QuadPattern {
    u32 segments{};
    u32 Vertices() const {
        return segments ? 4 * segments * 3 : 6;
    }
};

std::optional<QuadPattern> FindQuadPattern(const std::array<float, 6>& levels) {
    const float outer = levels[0];
    if (outer != levels[1] || outer != levels[2] || outer != levels[3] || outer < 1.0f ||
        outer > 64.0f || outer != std::floor(outer)) {
        return std::nullopt;
    }
    for (const float inner : {levels[4], levels[5]}) {
        if (inner < 1.0f || inner > 2.0f) {
            return std::nullopt;
        }
    }
    if (outer == 1.0f && levels[4] == 1.0f && levels[5] == 1.0f) {
        return QuadPattern{};
    }
    return QuadPattern{u32(outer)};
}

} // namespace

std::optional<FusedTessellation> FuseQuadTessellation(std::span<const u32> local_code,
                                                      std::span<const u32> hull_code,
                                                      std::span<const u32> domain_code) {
    auto local_opt = Parse(local_code);
    auto hull_opt = Parse(hull_code);
    auto domain_opt = Parse(domain_code);
    if (!local_opt || !hull_opt || !domain_opt) {
        FUSE_FAIL();
    }
    Module& ls = *local_opt;
    Module& ds = *domain_opt;
    const auto levels = ConstantLevels(*hull_opt);
    if (!levels) {
        FUSE_FAIL();
    }
    bool quads = false, equal = false, ccw = false;
    for (const auto& inst : ds.insts) {
        if (inst.op != Op::OpExecutionMode || inst.words.size() < 2) {
            continue;
        }
        switch (spv::ExecutionMode(inst.words[1])) {
        case spv::ExecutionMode::Quads: quads = true; break;
        case spv::ExecutionMode::SpacingEqual: equal = true; break;
        case spv::ExecutionMode::VertexOrderCcw: ccw = true; break;
        case spv::ExecutionMode::PointMode: FUSE_FAIL();
        default: break;
        }
    }
    const auto pattern = quads && equal ? FindQuadPattern(*levels) : std::nullopt;
    if (!pattern) {
        FUSE_FAIL();
    }

    // Entry points and the interface variables to turn into private ones.
    u32 ls_main = 0, ds_main = 0;
    for (const auto& inst : ls.insts) {
        if (inst.op == Op::OpEntryPoint) {
            if (spv::ExecutionModel(inst.words[0]) != spv::ExecutionModel::Vertex) {
                FUSE_FAIL();
            }
            ls_main = inst.words[1];
        }
    }
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpEntryPoint) {
            if (spv::ExecutionModel(inst.words[0]) !=
                spv::ExecutionModel::TessellationEvaluation) {
                FUSE_FAIL();
            }
            ds_main = inst.words[1];
        }
    }
    if (!ls_main || !ds_main) {
        FUSE_FAIL();
    }
    u32 ls_vertex_index = 0, ls_instance_index = 0, ls_out_attrs = 0, ls_push = 0;
    u32 ls_out_type = 0;
    std::set<u32> ls_private;
    for (const auto& inst : ls.insts) {
        if (inst.op != Op::OpVariable) {
            continue;
        }
        const auto storage = spv::StorageClass(inst.words[2]);
        const u32 id = inst.words[1];
        if (storage == spv::StorageClass::PushConstant) {
            ls_push = id;
        } else if (storage == spv::StorageClass::Input || storage == spv::StorageClass::Output) {
            ls_private.insert(id);
            const auto builtin = BuiltIn(ls, id);
            if (builtin == u32(spv::BuiltIn::VertexIndex)) {
                ls_vertex_index = id;
            } else if (builtin == u32(spv::BuiltIn::InstanceIndex)) {
                ls_instance_index = id;
            } else if (builtin && builtin != u32(spv::BuiltIn::Position)) {
                FUSE_FAIL(); // base vertex and the like: not provided
            } else if (!builtin && storage == spv::StorageClass::Output) {
                if (ls_out_attrs) {
                    FUSE_FAIL();
                }
                ls_out_attrs = id;
                const Inst* pointer = Definition(ls, inst.words[0]);
                ls_out_type = pointer ? pointer->words[2] : 0;
            } else if (!builtin) {
                FUSE_FAIL(); // vertex attributes through fixed-function input
            }
        }
    }
    u32 ds_tess_coord = 0, ds_primitive_id = 0, ds_in_attrs = 0, ds_push = 0, ds_in_type = 0;
    std::set<u32> ds_private;
    for (const auto& inst : ds.insts) {
        if (inst.op != Op::OpVariable) {
            continue;
        }
        const auto storage = spv::StorageClass(inst.words[2]);
        const u32 id = inst.words[1];
        if (storage == spv::StorageClass::PushConstant) {
            ds_push = id;
        } else if (storage == spv::StorageClass::Input) {
            ds_private.insert(id);
            const auto builtin = BuiltIn(ds, id);
            if (builtin == u32(spv::BuiltIn::TessCoord)) {
                ds_tess_coord = id;
            } else if (builtin == u32(spv::BuiltIn::PrimitiveId)) {
                ds_primitive_id = id;
            } else if (!builtin && !ds_in_attrs) {
                ds_in_attrs = id;
                const Inst* pointer = Definition(ds, inst.words[0]);
                ds_in_type = pointer ? pointer->words[2] : 0;
            } else {
                FUSE_FAIL(); // patch constants or other inputs
            }
        }
    }
    if (!ls_out_attrs || !ds_in_attrs || !ds_tess_coord) {
        FUSE_FAIL();
    }
    // Local outputs: array of N vec4; domain inputs: array[32] of array[M] vec4.
    const auto ls_slots = ArrayLength(ls, ls_out_type);
    const Inst* ds_outer = Definition(ds, ds_in_type);
    const auto ds_slots = ds_outer && ds_outer->op == Op::OpTypeArray
                              ? ArrayLength(ds, ds_outer->words[1])
                              : std::nullopt;
    if (!ls_slots || !ds_slots) {
        FUSE_FAIL();
    }
    const u32 slots = std::min(*ls_slots, *ds_slots);

    // The domain stage reads only the patch's one control point: its private copy keeps only
    // that one instead of 32 (4 KiB per vertex).
    {
        u32 uses = 0, u32_one = 0, int_type = 0;
        Inst* outer = nullptr;
        for (auto& inst : ds.insts) {
            const bool defines = IsTypeOrGlobal(inst.op) &&
                                 inst.words.size() > ResultIndex(inst.op) &&
                                 inst.words[ResultIndex(inst.op)] == ds_in_type;
            if (defines) {
                outer = &inst;
                continue;
            }
            if (!IsDebug(inst.op)) { // names hold strings
                uses += u32(std::count(inst.words.begin(), inst.words.end(), ds_in_type));
            }
            if (inst.op == Op::OpTypeInt && inst.words[1] == 32 && inst.words[2] == 0) {
                int_type = inst.words[0];
            } else if (!outer && inst.op == Op::OpConstant && inst.words.size() == 3 &&
                       inst.words[0] == int_type && inst.words[2] == 1) {
                u32_one = inst.words[1];
            }
        }
        if (!outer || uses != 1 || !u32_one) {
            FUSE_FAIL(); // the input array type is shared, or there is no constant 1 before it
        }
        outer->words[2] = u32_one;
    }

    // The domain module keeps its ids; the local module's go above them.
    const u32 offset = ds.bound;
    u32 next_id = ds.bound + ls.bound;
    const auto new_id = [&] { return next_id++; };
    std::map<u32, u32> ls_map; // local id -> fused id, where it is not id + offset
    const auto remap = [&](u32& id) {
        const auto it = ls_map.find(id);
        id = it != ls_map.end() ? it->second : id + offset;
    };

    // Domain module: tessellation inputs become private variables.
    for (auto& inst : ds.insts) {
        if (inst.op == Op::OpTypePointer &&
            spv::StorageClass(inst.words[1]) == spv::StorageClass::Input) {
            inst.words[1] = u32(spv::StorageClass::Private);
        } else if (inst.op == Op::OpVariable && ds_private.contains(inst.words[1])) {
            inst.words[2] = u32(spv::StorageClass::Private);
        }
    }

    // Unique types of the domain module, to map the local module's onto.
    std::map<std::vector<u32>, u32> ds_types;
    u32 u32_type = 0, f32_type = 0, vec3_type = 0, vec4_type = 0, void_type = 0, void_fn = 0;
    for (const auto& inst : ds.insts) {
        if (IsUniqueType(inst.op)) {
            ds_types.emplace(TypeKey(inst), inst.words[0]);
        }
        if (inst.op == Op::OpTypeInt && inst.words[1] == 32 && inst.words[2] == 0) {
            u32_type = inst.words[0];
        } else if (inst.op == Op::OpTypeFloat && inst.words[1] == 32) {
            f32_type = inst.words[0];
        } else if (inst.op == Op::OpTypeVoid) {
            void_type = inst.words[0];
        }
    }
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpTypeVector && inst.words[1] == f32_type && inst.words[2] == 3) {
            vec3_type = inst.words[0];
        } else if (inst.op == Op::OpTypeVector && inst.words[1] == f32_type &&
                   inst.words[2] == 4) {
            vec4_type = inst.words[0];
        } else if (inst.op == Op::OpTypeFunction && inst.words.size() == 2 &&
                   inst.words[1] == void_type) {
            void_fn = inst.words[0];
        }
    }
    if (!u32_type || !f32_type || !vec3_type || !vec4_type || !void_type || !void_fn) {
        FUSE_FAIL();
    }
    u32 ds_glsl = 0;
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpExtInstImport) {
            ds_glsl = inst.words[0];
        }
    }

    // Local module: drop its entry point, debug info and push constant variable (the domain's
    // has the same layout); map unique types; interface variables become private.
    std::vector<Inst> ls_annotations, ls_globals, ls_functions, extra_imports;
    std::set<u32> capabilities;
    std::vector<std::vector<u32>> extensions;
    ls_map[ls_push] = ds_push;
    for (auto& inst : ls.insts) {
        switch (inst.op) {
        case Op::OpCapability:
            capabilities.insert(inst.words[0]);
            continue;
        case Op::OpExtension:
            extensions.push_back(inst.words);
            continue;
        case Op::OpExtInstImport:
            if (ds_glsl) {
                ls_map[inst.words[0]] = ds_glsl;
            } else {
                inst.words[0] += offset;
                extra_imports.push_back(inst);
            }
            continue;
        case Op::OpMemoryModel: case Op::OpEntryPoint: case Op::OpExecutionMode:
            continue;
        default:
            break;
        }
        if (IsDebug(inst.op) || inst.op == Op::OpLine || inst.op == Op::OpNoLine) {
            continue;
        }
        if (inst.op == Op::OpDecorate || inst.op == Op::OpMemberDecorate) {
            if (ls_private.contains(inst.words[0]) || inst.words[0] == ls_push) {
                continue;
            }
            remap(inst.words[0]);
            ls_annotations.push_back(inst);
            continue;
        }
        if (IsTypeOrGlobal(inst.op)) {
            if (inst.op == Op::OpVariable && inst.words[1] == ls_push) {
                continue;
            }
            if (inst.op == Op::OpTypePointer &&
                (spv::StorageClass(inst.words[1]) == spv::StorageClass::Input ||
                 spv::StorageClass(inst.words[1]) == spv::StorageClass::Output)) {
                inst.words[1] = u32(spv::StorageClass::Private);
            }
            if (inst.op == Op::OpVariable && ls_private.contains(inst.words[1])) {
                inst.words[2] = u32(spv::StorageClass::Private);
            }
            // Operands first (they refer to earlier, already mapped ids), the result last.
            const size_t result_index = ResultIndex(inst.op);
            const u32 result = inst.words[result_index];
            Inst copy = inst;
            if (!ForEachId(copy, [&](u32& id) {
                    if (&id != &copy.words[result_index]) {
                        remap(id);
                    }
                })) {
                FUSE_FAIL();
            }
            if (IsUniqueType(inst.op)) {
                const auto it = ds_types.find(TypeKey(copy));
                if (it != ds_types.end()) {
                    ls_map[result] = it->second;
                    continue;
                }
            }
            copy.words[result_index] = result + offset;
            if (IsUniqueType(copy.op)) {
                ds_types.emplace(TypeKey(copy), copy.words[result_index]);
            }
            ls_globals.push_back(std::move(copy));
            continue;
        }
        Inst copy = inst;
        if (!ForEachId(copy, remap)) {
            FUSE_FAIL();
        }
        ls_functions.push_back(std::move(copy));
    }
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpCapability) {
            capabilities.insert(inst.words[0]);
        } else if (inst.op == Op::OpExtension &&
                   std::find(extensions.begin(), extensions.end(), inst.words) ==
                       extensions.end()) {
            extensions.push_back(inst.words);
        }
    }
    capabilities.erase(u32(spv::Capability::Tessellation));
    capabilities.erase(u32(spv::Capability::TessellationPointSize));
    // Remove duplicate extensions coming from the local module.
    std::sort(extensions.begin(), extensions.end());
    extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());

    // New declarations: built-in inputs, constants, pointer types.
    std::vector<Inst> globals, annotations;
    const auto constant_u32 = [&](u32 value) {
        const u32 id = new_id();
        globals.push_back({Op::OpConstant, {u32_type, id, value}});
        return id;
    };
    const u32 input_u32 = new_id();
    globals.push_back({Op::OpTypePointer, {input_u32, u32(spv::StorageClass::Input), u32_type}});
    const u32 private_vec4 = new_id();
    globals.push_back(
        {Op::OpTypePointer, {private_vec4, u32(spv::StorageClass::Private), vec4_type}});
    const u32 vertex_index = new_id();
    globals.push_back({Op::OpVariable, {input_u32, vertex_index, u32(spv::StorageClass::Input)}});
    const u32 instance_index = new_id();
    globals.push_back(
        {Op::OpVariable, {input_u32, instance_index, u32(spv::StorageClass::Input)}});
    annotations.push_back({Op::OpDecorate, {vertex_index, u32(spv::Decoration::BuiltIn),
                                            u32(spv::BuiltIn::VertexIndex)}});
    annotations.push_back({Op::OpDecorate, {instance_index, u32(spv::Decoration::BuiltIn),
                                            u32(spv::BuiltIn::InstanceIndex)}});
    const u32 zero = constant_u32(0), one = constant_u32(1);
    std::vector<u32> slot_ids;
    for (u32 i = 0; i < slots; ++i) {
        slot_ids.push_back(i == 0 ? zero : i == 1 ? one : constant_u32(i));
    }
    std::map<u32, u32> floats; // bits -> constant
    const auto constant_f32 = [&](float value) {
        const auto [it, is_new] = floats.try_emplace(std::bit_cast<u32>(value));
        if (is_new) {
            it->second = new_id();
            globals.push_back({Op::OpConstant, {f32_type, it->second, it->first}});
        }
        return it->second;
    };
    const u32 f_zero = constant_f32(0.0f);
    u32 bool_type = 0;
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpTypeBool) {
            bool_type = inst.words[0];
        }
    }
    if (!bool_type) {
        bool_type = new_id();
        globals.push_back({Op::OpTypeBool, {bool_type}});
    }

    // main: the point's index for the local stage, its outputs as the patch's control point,
    // the vertex's tessellation coordinate, then the domain stage.
    std::vector<Inst> main;
    const u32 main_id = new_id();
    main.push_back({Op::OpFunction, {void_type, main_id, 0, void_fn}});
    main.push_back({Op::OpLabel, {new_id()}});
    const u32 point = new_id();
    main.push_back({Op::OpLoad, {u32_type, point, instance_index}});
    if (ls_vertex_index) {
        main.push_back({Op::OpStore, {ls_vertex_index + offset, point}});
    }
    if (ls_instance_index) {
        main.push_back({Op::OpStore, {ls_instance_index + offset, zero}});
    }
    main.push_back({Op::OpFunctionCall, {void_type, new_id(), ls_main + offset}});
    for (u32 i = 0; i < slots; ++i) {
        const u32 src = new_id(), value = new_id(), dst = new_id();
        main.push_back({Op::OpAccessChain, {private_vec4, src, ls_out_attrs + offset, slot_ids[i]}});
        main.push_back({Op::OpLoad, {vec4_type, value, src}});
        main.push_back({Op::OpAccessChain, {private_vec4, dst, ds_in_attrs, zero, slot_ids[i]}});
        main.push_back({Op::OpStore, {dst, value}});
    }
    const u32 vertex = new_id();
    main.push_back({Op::OpLoad, {u32_type, vertex, vertex_index}});
    const auto op = [&](Op code, u32 type, std::vector<u32> operands) {
        const u32 id = new_id();
        operands.insert(operands.begin(), {type, id});
        main.push_back({code, std::move(operands)});
        return id;
    };
    u32 u, v;
    if (!pattern->segments) {
        // Vertices 0..5: (0,0) (1,0) (1,1) (0,0) (1,1) (0,1), or the reverse order.
        const auto bit = [&](u32 mask) {
            const u32 shifted = op(Op::OpShiftRightLogical, u32_type, {constant_u32(mask), vertex});
            return op(Op::OpConvertUToF, f32_type,
                      {op(Op::OpBitwiseAnd, u32_type, {shifted, one})});
        };
        u = bit(ccw ? 0b100110 : 0b010110);
        v = bit(ccw ? 0b110010 : 0b110100);
    } else {
        // Triangle t is outer-edge points t and t + 1 (counted around from (0,0) through (1,0),
        // (1,1), (0,1)) and the centre.
        const u32 s = pattern->segments;
        const u32 triangle = op(Op::OpUDiv, u32_type, {vertex, constant_u32(3)});
        const u32 corner = op(Op::OpUMod, u32_type, {vertex, constant_u32(3)});
        const u32 next = op(Op::OpIEqual, bool_type, {corner, ccw ? constant_u32(2) : one});
        const u32 centre = op(Op::OpIEqual, bool_type, {corner, ccw ? one : constant_u32(2)});
        const u32 point = op(Op::OpUMod, u32_type,
                             {op(Op::OpIAdd, u32_type,
                                 {triangle, op(Op::OpSelect, u32_type, {next, one, zero})}),
                              constant_u32(4 * s)});
        const u32 edge = op(Op::OpUDiv, u32_type, {point, constant_u32(s)});
        const u32 t = op(Op::OpFDiv, f32_type,
                         {op(Op::OpConvertUToF, f32_type,
                             {op(Op::OpUMod, u32_type, {point, constant_u32(s)})}),
                          constant_f32(float(s))});
        const u32 f_one = constant_f32(1.0f);
        const u32 rest = op(Op::OpFSub, f32_type, {f_one, t});
        const auto by_edge = [&](u32 e0, u32 e1, u32 e2, u32 e3) {
            const auto is = [&](u32 e) {
                return op(Op::OpIEqual, bool_type, {edge, constant_u32(e)});
            };
            const u32 hi = op(Op::OpSelect, f32_type, {is(2), e2, e3});
            const u32 lo = op(Op::OpSelect, f32_type, {is(1), e1, hi});
            const u32 value = op(Op::OpSelect, f32_type, {is(0), e0, lo});
            return op(Op::OpSelect, f32_type, {centre, constant_f32(0.5f), value});
        };
        u = by_edge(t, f_one, rest, f_zero);
        v = by_edge(f_zero, t, f_one, rest);
    }
    const u32 tess_coord = new_id();
    main.push_back({Op::OpCompositeConstruct, {vec3_type, tess_coord, u, v, f_zero}});
    main.push_back({Op::OpStore, {ds_tess_coord, tess_coord}});
    if (ds_primitive_id) {
        main.push_back({Op::OpStore, {ds_primitive_id, zero}});
    }
    main.push_back({Op::OpFunctionCall, {void_type, new_id(), ds_main}});
    main.push_back({Op::OpReturn, {}});
    main.push_back({Op::OpFunctionEnd, {}});

    // Assemble in the logical layout.
    std::vector<u32> out{spv::MagicNumber, std::max(ls.version, ds.version), 0, 0, 0};
    const auto emit = [&](const Inst& inst) {
        out.push_back(u32(inst.words.size() + 1) << 16 | u32(inst.op));
        out.insert(out.end(), inst.words.begin(), inst.words.end());
    };
    for (const u32 capability : capabilities) {
        emit({Op::OpCapability, {capability}});
    }
    for (const auto& extension : extensions) {
        emit({Op::OpExtension, extension});
    }
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpExtInstImport) {
            emit(inst);
        }
    }
    for (const auto& inst : extra_imports) {
        emit(inst);
    }
    for (const auto& inst : ds.insts) {
        if (inst.op == Op::OpMemoryModel) {
            emit(inst);
        }
    }
    // Every global variable is part of the interface (SPIR-V 1.4 and later).
    std::vector<u32> interface;
    for (const auto* list : {&ds.insts, &ls_globals, &globals}) {
        for (const auto& inst : *list) {
            if (inst.op == Op::OpVariable &&
                spv::StorageClass(inst.words[2]) != spv::StorageClass::Function) {
                interface.push_back(inst.words[1]);
            }
        }
    }
    std::vector<u32> entry{u32(spv::ExecutionModel::Vertex), main_id};
    const std::string_view name{"main"};
    for (size_t i = 0; i <= name.size(); i += 4) {
        u32 word = 0;
        for (size_t j = 0; j < 4 && i + j < name.size(); ++j) {
            word |= u32(u8(name[i + j])) << (8 * j);
        }
        entry.push_back(word);
    }
    entry.insert(entry.end(), interface.begin(), interface.end());
    emit({Op::OpEntryPoint, entry});
    for (const auto& inst : ds.insts) {
        if ((inst.op == Op::OpExecutionMode || inst.op == Op::OpExecutionModeId) &&
            !IsTessellationMode(inst.words[1])) {
            Inst mode = inst;
            mode.words[0] = main_id;
            emit(mode);
        }
    }
    for (const auto& inst : ds.insts) {
        if (IsDebug(inst.op)) {
            emit(inst);
        }
    }
    for (const auto& inst : ds.insts) {
        if ((inst.op == Op::OpDecorate || inst.op == Op::OpMemberDecorate) &&
            !ds_private.contains(inst.words[0])) {
            emit(inst);
        }
    }
    for (const auto& inst : ls_annotations) {
        emit(inst);
    }
    for (const auto& inst : annotations) {
        emit(inst);
    }
    for (const auto& inst : ds.insts) {
        if (IsTypeOrGlobal(inst.op)) {
            emit(inst);
        }
    }
    for (const auto& inst : ls_globals) {
        emit(inst);
    }
    for (const auto& inst : globals) {
        emit(inst);
    }
    bool in_function = false;
    for (const auto& inst : ds.insts) {
        in_function |= inst.op == Op::OpFunction;
        if (in_function) {
            emit(inst);
        }
    }
    for (const auto& inst : ls_functions) {
        emit(inst);
    }
    for (const auto& inst : main) {
        emit(inst);
    }
    out[3] = next_id;
    return FusedTessellation{std::move(out), pattern->Vertices()};
}

} // namespace Vulkan
