// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include <stdexcept>

namespace Shader::Backend::SPIRV {
namespace {
u32 AttributeOffset(IR::Attribute attr, u32 component) {
  if (component >= 4)
    throw std::runtime_error("Geometry component outside vec4");
  if (attr == IR::Attribute::Position0)
    return component;
  if (IR::IsParam(attr))
    return 4 + (u32(attr) - u32(IR::Attribute::Param0)) * 4 + component;
  throw std::runtime_error("Geometry compute attribute is not supported yet");
}
Id WordPointer(EmitContext &ctx, Id buffer, Id word) {
  return ctx.OpAccessChain(
      ctx.TypePointer(spv::StorageClass::StorageBuffer, ctx.U32[1]), buffer,
      ctx.u32_zero_value, word);
}
// Caller has an active basic block. Continue only for valid
// invocations/allocations.
void ReturnUnless(EmitContext &ctx, Id valid) {
  const Id reject = ctx.OpLabel(), next = ctx.OpLabel();
  ctx.OpSelectionMerge(next, spv::SelectionControlMask::MaskNone);
  ctx.OpBranchConditional(valid, next, reject);
  ctx.AddLabel(reject);
  ctx.OpReturn();
  ctx.AddLabel(next);
}
} // namespace

void EmitContext::DefineExportCompute() {
  const u32 stride = runtime_info.hw.es.vertex_data_size;
  if (stride == 0 || stride > 4 + IR::NumParams * 4 || info.has_fetch_shader ||
      info.uses_lane_id || info.uses_group_ballot || info.uses_group_shuffle ||
      info.uses_group_quad || info.shared_types != IR::Type::Void)
    throw std::runtime_error(
        "Export compute requires independent manual-fetch vertices");
  for (u32 attr = 0; attr < IR::NumAttributes; ++attr) {
    const auto semantic = IR::Attribute(attr);
    for (u32 comp = 0; comp < 4; ++comp)
      if (info.stores.Get(semantic, comp) &&
          AttributeOffset(semantic, comp) >= stride)
        throw std::runtime_error("Export attribute exceeds ES ring stride");
  }
  geometry->attribute_dwords = stride;
  geometry->vertex_stride_dwords = stride;
  geometry->primitive_stride_dwords = stride;
  geometry->input_binding = binding.unified;
  geometry_input = DefineBuffer(false, false, 2, BufferType::Guest, U32[1]).id;
  ++binding.unified;
  geometry->output_binding = binding.unified;
  geometry_output = DefineBuffer(true, false, 2, BufferType::Guest, U32[1]).id;
  ++binding.unified;
  Name(geometry_input, "export_draw_input");
  Name(geometry_output, "export_point_attributes");
}

void EmitContext::ExportPrologue() {
  geometry_primitive =
      OpCompositeExtract(U32[1], OpLoad(U32[3], geometry_invocation), 0u);
  const Id output_words = OpArrayLength(U32[1], geometry_output, 0u);
  ReturnUnless(*this, OpUGreaterThanEqual(U1[1], output_words, ConstU32(4u)));
  // Only invocation zero owns the header. Initialize it even for invalid/empty
  // draws.
  const auto write_header = [&](Id count, Id points_per_instance) {
    const Id store = OpLabel(), next = OpLabel();
    const Id first = OpIEqual(U1[1], geometry_primitive, u32_zero_value);
    OpSelectionMerge(next, spv::SelectionControlMask::MaskNone);
    OpBranchConditional(first, store, next);
    AddLabel(store);
    for (u32 word = 0; word < 4; ++word)
      OpStore(WordPointer(*this, geometry_output, ConstU32(word)),
              word == 0   ? count
              : word == 1 ? points_per_instance
                          : u32_zero_value);
    OpBranch(next);
    AddLabel(next);
  };
  write_header(u32_zero_value, u32_zero_value);
  const Id input_words = OpArrayLength(U32[1], geometry_input, 0u);
  ReturnUnless(*this, OpUGreaterThanEqual(U1[1], input_words, ConstU32(8u)));
  const auto control = [&](u32 word) {
    return OpLoad(U32[1], WordPointer(*this, geometry_input, ConstU32(word)));
  };
  const Id vertices = control(0), instances = control(1),
           index_flag = control(4);
  export_base_vertex = control(2);
  export_base_instance = control(3);
  ReturnUnless(*this, OpULessThanEqual(U1[1], index_flag, ConstU32(1u)));
  const Id indexed = OpINotEqual(U1[1], index_flag, u32_zero_value);
  const Id index_capacity = OpISub(U32[1], input_words, ConstU32(8u));
  ReturnUnless(*this,
               OpLogicalOr(U1[1], OpLogicalNot(U1[1], indexed),
                           OpULessThanEqual(U1[1], vertices, index_capacity)));
  ReturnUnless(*this, OpINotEqual(U1[1], vertices, u32_zero_value));
  const Id capacity = OpUDiv(U32[1], OpISub(U32[1], output_words, ConstU32(4u)),
                             ConstU32(geometry->attribute_dwords));
  // Compare using division before multiplication, so huge instance counts
  // cannot wrap.
  const Id too_large =
      OpUGreaterThan(U1[1], instances, OpUDiv(U32[1], capacity, vertices));
  const Id count = OpSelect(U32[1], too_large, capacity,
                            OpIMul(U32[1], vertices, instances));
  write_header(count, vertices);
  ReturnUnless(*this, OpULessThan(U1[1], geometry_primitive, count));
  const Id local_vertex = OpUMod(U32[1], geometry_primitive, vertices);
  export_instance = OpIAdd(U32[1], export_base_instance,
                           OpUDiv(U32[1], geometry_primitive, vertices));
  // Branch around the index load: nonindexed input contains no index array.
  const Id from_index = OpLabel(), sequential = OpLabel(), next = OpLabel();
  OpSelectionMerge(next, spv::SelectionControlMask::MaskNone);
  OpBranchConditional(indexed, from_index, sequential);
  AddLabel(from_index);
  const Id index =
      OpLoad(U32[1], WordPointer(*this, geometry_input,
                                 OpIAdd(U32[1], ConstU32(8u), local_vertex)));
  OpBranch(next);
  AddLabel(sequential);
  OpBranch(next);
  AddLabel(next);
  export_vertex =
      OpIAdd(U32[1], OpPhi(U32[1], index, from_index, local_vertex, sequential),
             export_base_vertex);
  geometry_record = OpIAdd(
      U32[1], ConstU32(4u),
      OpIMul(U32[1], geometry_primitive, ConstU32(geometry->attribute_dwords)));
  for (u32 word = 0; word < geometry->attribute_dwords; ++word)
    OpStore(WordPointer(*this, geometry_output,
                        OpIAdd(U32[1], geometry_record, ConstU32(word))),
            u32_zero_value);
}

void EmitContext::DefineGeometryCompute() {
  if (hw_stage == HwStage::Export && sw_stage == SwStage::Vertex) {
    DefineExportCompute();
    return;
  }
  const auto &gs = runtime_info.hw.gs;
  if (sw_stage != SwStage::Geometry ||
      gs.in_primitive != AmdGpu::PrimitiveType::PointList ||
      gs.num_invocations != 1 || gs.output_vertices == 0 ||
      gs.output_vertices > 256 || gs.in_vertex_data_size == 0 ||
      info.uses_lane_id || info.uses_group_ballot || info.uses_group_shuffle ||
      info.uses_group_quad)
    throw std::runtime_error(
        "Geometry compute currently requires independent point invocations");
  geometry->attribute_dwords = 4;
  for (u32 attr = 0; attr < IR::NumAttributes; ++attr) {
    const auto semantic = IR::Attribute(attr);
    if (!info.stores.GetAny(semantic))
      continue;
    geometry->attribute_dwords =
        std::max(geometry->attribute_dwords, AttributeOffset(semantic, 3) + 1);
  }
  geometry->vertex_stride_dwords = 1 + geometry->attribute_dwords;
  geometry->primitive_stride_dwords =
      2 + gs.output_vertices * geometry->vertex_stride_dwords;
  geometry->input_binding = binding.unified;
  geometry_input = DefineBuffer(false, false, 2, BufferType::Guest, U32[1]).id;
  ++binding.unified;
  geometry->output_binding = binding.unified;
  geometry_output = DefineBuffer(true, false, 2, BufferType::Guest, U32[1]).id;
  ++binding.unified;
  Name(geometry_input, "geometry_es_input");
  Name(geometry_output, "geometry_expanded_output");
  const Id attributes = TypeArray(F32[1], ConstU32(geometry->attribute_dwords));
  geometry_attributes =
      DefineVariable(attributes, std::nullopt, spv::StorageClass::Private,
                     ConstantNull(attributes));
  geometry_count = DefineVariable(U32[1], std::nullopt,
                                  spv::StorageClass::Private, u32_zero_value);
  geometry_strip = DefineVariable(U32[1], std::nullopt,
                                  spv::StorageClass::Private, u32_zero_value);
}

void EmitContext::GeometryPrologue() {
  if (hw_stage == HwStage::Export) {
    ExportPrologue();
    return;
  }
  geometry_primitive =
      OpCompositeExtract(U32[1], OpLoad(U32[3], geometry_invocation), 0u);
  const Id output_capacity =
      OpUDiv(U32[1], OpArrayLength(U32[1], geometry_output, 0u),
             ConstU32(geometry->primitive_stride_dwords));
  ReturnUnless(*this, OpULessThan(U1[1], geometry_primitive, output_capacity));
  geometry_record = OpIMul(U32[1], geometry_primitive,
                           ConstU32(geometry->primitive_stride_dwords));
  // Retired/inactive invocations must not expose a previous draw's emitted
  // vertices.
  OpStore(WordPointer(*this, geometry_output, geometry_record), u32_zero_value);
  OpStore(WordPointer(*this, geometry_output,
                      OpIAdd(U32[1], geometry_record, ConstU32(1u))),
          u32_zero_value);
  const Id input_words = OpArrayLength(U32[1], geometry_input, 0u);
  ReturnUnless(*this, OpUGreaterThanEqual(U1[1], input_words, ConstU32(4u)));
  const Id input_capacity =
      OpUDiv(U32[1], OpISub(U32[1], input_words, ConstU32(4u)),
             ConstU32(runtime_info.hw.gs.in_vertex_data_size));
  const Id count =
      OpLoad(U32[1], WordPointer(*this, geometry_input, u32_zero_value));
  const Id points_per_instance =
      OpLoad(U32[1], WordPointer(*this, geometry_input, ConstU32(1u)));
  // Vulkan PrimitiveId resets for each draw instance, independently of ES
  // vertex indices.
  const Id instanced = OpINotEqual(U1[1], points_per_instance, u32_zero_value);
  const Id divisor =
      OpSelect(U32[1], instanced, points_per_instance, ConstU32(1u));
  geometry_primitive_id =
      OpSelect(U32[1], instanced, OpUMod(U32[1], geometry_primitive, divisor),
               geometry_primitive);
  const Id fits_input = OpULessThan(U1[1], geometry_primitive, input_capacity);
  const Id active = OpULessThan(U1[1], geometry_primitive, count);
  ReturnUnless(*this, OpLogicalAnd(U1[1], active, fits_input));
}

Id EmitContext::GeometryLoadAttribute(IR::Attribute attr, u32 comp,
                                      u32 vertex) {
  if (hw_stage == HwStage::Export)
    throw std::runtime_error(
        "Export compute cannot read fixed-function vertex attributes");
  const u32 offset = AttributeOffset(attr, comp);
  if (vertex != 0 || offset >= runtime_info.hw.gs.in_vertex_data_size)
    throw std::runtime_error("Geometry input outside captured point layout");
  const Id index =
      OpIAdd(U32[1], ConstU32(4 + offset),
             OpIMul(U32[1], geometry_primitive,
                    ConstU32(runtime_info.hw.gs.in_vertex_data_size)));
  return OpBitcast(F32[1],
                   OpLoad(U32[1], WordPointer(*this, geometry_input, index)));
}

void EmitContext::GeometrySetAttribute(IR::Attribute attr, u32 comp, Id value) {
  const u32 offset = AttributeOffset(attr, comp);
  if (offset >= geometry->attribute_dwords)
    throw std::runtime_error(
        "Geometry output outside collected attribute layout");
  if (hw_stage == HwStage::Export) {
    OpStore(WordPointer(*this, geometry_output,
                        OpIAdd(U32[1], geometry_record, ConstU32(offset))),
            OpBitcast(U32[1], value));
    return;
  }
  OpStore(OpAccessChain(TypePointer(spv::StorageClass::Private, F32[1]),
                        geometry_attributes, ConstU32(offset)),
          value);
}

void EmitContext::GeometryEmitVertex() {
  const Id count = OpLoad(U32[1], geometry_count);
  const Id fits =
      OpULessThan(U1[1], count, ConstU32(runtime_info.hw.gs.output_vertices));
  const Id store = OpLabel(), overflow = OpLabel(), next = OpLabel();
  OpSelectionMerge(next, spv::SelectionControlMask::MaskNone);
  OpBranchConditional(fits, store, overflow);
  AddLabel(overflow);
  OpStore(WordPointer(*this, geometry_output,
                      OpIAdd(U32[1], geometry_record, ConstU32(1u))),
          ConstU32(1u));
  OpBranch(next);
  AddLabel(store);
  const Id record =
      OpIAdd(U32[1], OpIAdd(U32[1], geometry_record, ConstU32(2u)),
             OpIMul(U32[1], count, ConstU32(geometry->vertex_stride_dwords)));
  OpStore(WordPointer(*this, geometry_output, record),
          OpLoad(U32[1], geometry_strip));
  for (u32 component = 0; component < geometry->attribute_dwords; ++component) {
    const Id value = OpLoad(
        F32[1], OpAccessChain(TypePointer(spv::StorageClass::Private, F32[1]),
                              geometry_attributes, ConstU32(component)));
    OpStore(WordPointer(*this, geometry_output,
                        OpIAdd(U32[1], record, ConstU32(1 + component))),
            OpBitcast(U32[1], value));
  }
  const Id emitted = OpIAdd(U32[1], count, ConstU32(1u));
  OpStore(geometry_count, emitted);
  OpStore(WordPointer(*this, geometry_output, geometry_record), emitted);
  OpBranch(next);
  AddLabel(next);
}

void EmitContext::GeometryEndPrimitive() {
  // The start index identifies the strip and lets replay derive winding without
  // rescanning earlier vertices. Empty cuts have no emitted vertices.
  OpStore(geometry_strip, OpLoad(U32[1], geometry_count));
}
} // namespace Shader::Backend::SPIRV
