// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include <sirit/sirit.h>
#include <stdexcept>

namespace Shader::Backend::SPIRV {
namespace {
using Sirit::Id;

class ReplayModule final : public Sirit::Module {
  public:
    ReplayModule(const Profile &profile, const GeometryComputeLayout &layout, u32 max_vertices,
                 u32 capture_binding, bool last_provoking_vertex, u32 input_binding,
                 bool native_last_provoking, bool remap_negative_depth, u32 flat_corner_mask)
        : Module(profile.supported_spirv) {
        if (max_vertices < 3 || max_vertices > 256 || layout.attribute_dwords < 4 ||
            layout.attribute_dwords > 4 * (IR::NumParams + 1) || layout.attribute_dwords % 4 ||
            layout.vertex_stride_dwords != layout.attribute_dwords + 1 ||
            layout.primitive_stride_dwords != 2 + max_vertices * layout.vertex_stride_dwords)
            throw std::runtime_error("Invalid triangle-strip replay layout");
        AddCapability(spv::Capability::Shader);
        SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
        void_type = TypeVoid();
        bool_type = TypeBool();
        uint_type = TypeInt(32, false);
        float_type = TypeFloat(32);
        vec4_type = TypeVector(float_type, 4);
        zero = U(0);
        const Id vertex = Variable(uint_type, spv::StorageClass::Input);
        const Id instance = Variable(uint_type, spv::StorageClass::Input);
        Decorate(vertex, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
        Decorate(instance, spv::Decoration::BuiltIn, spv::BuiltIn::InstanceIndex);
        const Id position = Variable(vec4_type, spv::StorageClass::Output);
        Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
        std::vector<Id> parameters;
        for (u32 index = 0; index < layout.attribute_dwords / 4 - 1; ++index) {
            const Id output = Variable(vec4_type, spv::StorageClass::Output);
            Decorate(output, spv::Decoration::Location, index);
            parameters.push_back(output);
        }
        // Lowered per-vertex fragment inputs: every vertex of a triangle writes the values of
        // all three of its corners, so flat interpolation is exact whatever vertex provokes.
        std::vector<std::pair<u32, std::array<Id, 2>>> corner_outputs;
        for (u32 index = 0; index < parameters.size(); ++index) {
            if (!(flat_corner_mask & (1u << index))) {
                continue;
            }
            if (index >= FlatCornerLocationStride)
                throw std::runtime_error("Per-vertex attribute beyond flat corner locations");
            std::array<Id, 2> outputs;
            for (u32 corner = 1; corner < 3; ++corner) {
                outputs[corner - 1] = Variable(vec4_type, spv::StorageClass::Output);
                Decorate(outputs[corner - 1], spv::Decoration::Location,
                         FlatCornerLocation(index, corner));
            }
            corner_outputs.emplace_back(index, outputs);
        }
        const Id array = TypeRuntimeArray(uint_type);
        Decorate(array, spv::Decoration::ArrayStride, 4u);
        const Id block = TypeStruct(array);
        Decorate(block, spv::Decoration::Block);
        MemberDecorate(block, 0, spv::Decoration::Offset, 0u);
        data = Variable(block, spv::StorageClass::StorageBuffer);
        Decorate(data, spv::Decoration::Binding, input_binding);
        Decorate(data, spv::Decoration::DescriptorSet, 0u);
        Decorate(data, spv::Decoration::NonWritable);
        Name(data, "geometry_expanded_vertices");
        Id capture{};
        if (capture_binding != ~0u) {
            if (capture_binding == input_binding)
                throw std::runtime_error("Capture binding aliases input");
            capture = Variable(block, spv::StorageClass::StorageBuffer);
            Decorate(capture, spv::Decoration::Binding, capture_binding);
            Decorate(capture, spv::Decoration::DescriptorSet, 0u);
            Name(capture, "geometry_vertex_readback");
        }

        const Id main =
            OpFunction(void_type, spv::FunctionControlMask::MaskNone, TypeFunction(void_type));
        AddLabel(OpLabel());
        const Id outside = ConstantComposite(
            vec4_type, std::array<Id, 4>{Constant(float_type, 2.f), Constant(float_type, 2.f),
                                         Constant(float_type, 2.f), Constant(float_type, 1.f)});
        // Rejected vertices remain outside the clip volume, even after an early return.
        OpStore(position, outside);
        for (Id parameter : parameters)
            OpStore(parameter, ConstantNull(vec4_type));
        for (const auto& [index, outputs] : corner_outputs)
            for (Id output : outputs)
                OpStore(output, ConstantNull(vec4_type));
        const Id primitive = OpLoad(uint_type, instance);
        const Id vindex = OpLoad(uint_type, vertex);
        const Id capacity = OpUDiv(uint_type, OpArrayLength(uint_type, data, 0u),
                                   U(layout.primitive_stride_dwords));
        Require(OpLogicalAnd(bool_type, OpULessThan(bool_type, primitive, capacity),
                             OpULessThan(bool_type, vindex, U((max_vertices - 2) * 3))));
        const Id base = OpIMul(uint_type, primitive, U(layout.primitive_stride_dwords));
        const Id triangle = OpUDiv(uint_type, vindex, U(3));
        const Id third = OpIAdd(uint_type, triangle, U(2));
        Require(OpULessThan(bool_type, third, Load(base)));
        const auto record = [&](Id index) {
            return OpIAdd(uint_type, OpIAdd(uint_type, base, U(2)),
                          OpIMul(uint_type, index, U(layout.vertex_stride_dwords)));
        };
        const Id strip_start = Load(record(triangle));
        const Id second_start = Load(record(OpIAdd(uint_type, triangle, U(1))));
        const Id third_start = Load(record(third));
        Require(OpLogicalAnd(bool_type, OpULessThanEqual(bool_type, strip_start, triangle),
                             OpLogicalAnd(bool_type, OpIEqual(bool_type, strip_start, second_start),
                                          OpIEqual(bool_type, strip_start, third_start))));

        // Each strip starts with even winding. Global triangle parity is wrong if
        // an earlier strip emitted an odd number of vertices before cutting.
        const Id parity = OpBitwiseAnd(uint_type, OpISub(uint_type, triangle, strip_start), U(1));
        Id corner = OpUMod(uint_type, vindex, U(3));
        // Vulkan's fallback uses the first corner. A cyclic rotation puts the
        // guest's last corner there without reversing the triangle winding.
        if (last_provoking_vertex && !native_last_provoking)
            corner = OpUMod(uint_type, OpIAdd(uint_type, corner, U(2)), U(3));
        // Keep the provoking corner fixed while reversing odd triangles. First-
        // vertex convention uses (0,2,1); last-vertex convention uses (1,0,2).
        const Id movable = last_provoking_vertex ? OpULessThan(bool_type, corner, U(2))
                                                 : OpUGreaterThan(bool_type, corner, U(0));
        const Id flipped = last_provoking_vertex ? OpBitwiseXor(uint_type, corner, U(1))
                                                 : OpISub(uint_type, U(3), corner);
        const Id flip = OpLogicalAnd(bool_type, OpIEqual(bool_type, parity, U(1)), movable);
        const Id adjusted = OpSelect(uint_type, flip, flipped, corner);
        const Id selected = record(OpIAdd(uint_type, triangle, adjusted));
        const auto attribute_of = [&](Id vertex_record, u32 offset) {
            std::array<Id, 4> components;
            for (u32 comp = 0; comp < 4; ++comp)
                components[comp] = OpBitcast(
                    float_type, Load(OpIAdd(uint_type, vertex_record, U(1 + offset + comp))));
            return OpCompositeConstruct(vec4_type, components);
        };
        const auto attribute = [&](u32 offset) { return attribute_of(selected, offset); };
        // The guest's corner k of this triangle: strip order with the odd-triangle reversal,
        // without the provoking rotation (that only places the guest's provoking corner).
        const auto guest_corner = [&](u32 k) {
            Id adjusted_k = U(k);
            const bool movable = last_provoking_vertex ? k < 2 : k > 0;
            if (movable) {
                const u32 flipped_k = last_provoking_vertex ? (k ^ 1u) : 3 - k;
                adjusted_k = OpSelect(uint_type, OpIEqual(bool_type, parity, U(1)), U(flipped_k),
                                      U(k));
            }
            return record(OpIAdd(uint_type, triangle, adjusted_k));
        };
        Id clip_position = attribute(0);
        if (remap_negative_depth) {
            const Id z = OpCompositeExtract(float_type, clip_position, 2u);
            const Id w = OpCompositeExtract(float_type, clip_position, 3u);
            const Id mapped_z = OpFMul(float_type, OpFAdd(float_type, z, w),
                                       Constant(float_type, 0.5f));
            clip_position = OpCompositeInsert(vec4_type, mapped_z, clip_position, 2u);
        }
        OpStore(position, clip_position);
        for (u32 i = 0; i < parameters.size(); ++i)
            OpStore(parameters[i], attribute((i + 1) * 4));
        if (!corner_outputs.empty()) {
            const std::array<Id, 3> corners{guest_corner(0), guest_corner(1), guest_corner(2)};
            for (const auto& [index, outputs] : corner_outputs) {
                OpStore(parameters[index], attribute_of(corners[0], (index + 1) * 4));
                OpStore(outputs[0], attribute_of(corners[1], (index + 1) * 4));
                OpStore(outputs[1], attribute_of(corners[2], (index + 1) * 4));
            }
        }
        if (capture_binding != ~0u) {
            const u32 words_per_instance = (max_vertices - 2) * 3 * layout.attribute_dwords;
            const Id capacity =
                OpUDiv(uint_type, OpArrayLength(uint_type, capture, 0u), U(words_per_instance));
            Require(OpULessThan(bool_type, primitive, capacity));
            const Id start = OpIAdd(uint_type, OpIMul(uint_type, primitive, U(words_per_instance)),
                                    OpIMul(uint_type, vindex, U(layout.attribute_dwords)));
            for (u32 attr = 0; attr < layout.attribute_dwords / 4; ++attr) {
                const Id value = OpLoad(vec4_type, attr == 0 ? position : parameters[attr - 1]);
                for (u32 comp = 0; comp < 4; ++comp) {
                    const Id ptr =
                        OpAccessChain(TypePointer(spv::StorageClass::StorageBuffer, uint_type),
                                      capture, zero, OpIAdd(uint_type, start, U(attr * 4 + comp)));
                    OpStore(ptr, OpBitcast(uint_type, OpCompositeExtract(float_type, value, comp)));
                }
            }
        }
        OpReturn();
        OpFunctionEnd();
        AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", interfaces);
    }

  private:
    Id Variable(Id type, spv::StorageClass storage) {
        const Id result = AddGlobalVariable(TypePointer(storage, type), storage);
        interfaces.push_back(result);
        return result;
    }
    Id U(u32 value) { return Constant(uint_type, value); }
    Id Load(Id word) {
        return OpLoad(uint_type,
                      OpAccessChain(TypePointer(spv::StorageClass::StorageBuffer, uint_type), data,
                                    zero, word));
    }
    void Require(Id valid) {
        const Id reject = OpLabel(), next = OpLabel();
        OpSelectionMerge(next, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(valid, next, reject);
        AddLabel(reject);
        OpReturn();
        AddLabel(next);
    }
    Id void_type, bool_type, uint_type, float_type, vec4_type, zero, data;
    std::vector<Id> interfaces;
};
} // namespace

std::vector<u32> EmitGeometryReplayVertex(const Profile &profile,
                                          const GeometryComputeLayout &geometry, u32 max_vertices,
                                          u32 capture_binding, bool last_provoking_vertex,
                                          u32 input_binding, bool native_last_provoking,
                                          bool remap_negative_depth, u32 flat_corner_mask) {
    return ReplayModule(profile, geometry, max_vertices, capture_binding, last_provoking_vertex,
                        input_binding, native_last_provoking, remap_negative_depth,
                        flat_corner_mask)
        .Assemble();
}
} // namespace Shader::Backend::SPIRV
