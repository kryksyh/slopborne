// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <sirit/sirit.h>

#include "shader_recompiler/backend/spirv/emit_spirv_solid_frag.h"

namespace Shader::Backend::SPIRV {

std::vector<u32> EmitSolidFragmentShader(std::span<const SolidOutput> outputs,
                                         const Info::StandIn::Target* textured,
                                         u32 textured_location, bool alpha_test) {
    Sirit::Module module{0x00010300};
    module.AddCapability(spv::Capability::Shader);
    module.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const auto void_id = module.TypeVoid();
    const auto bool_id = module.TypeBool();
    const auto float_id = module.TypeFloat(32);
    const auto vec2_id = module.TypeVector(float_id, 2);
    const auto vec4_id = module.TypeVector(float_id, 4);
    const auto output_ptr = module.TypePointer(spv::StorageClass::Output, vec4_id);
    std::vector<Sirit::Id> variables, values, interfaces;
    for (const auto& output : outputs) {
        const auto variable = module.AddGlobalVariable(output_ptr, spv::StorageClass::Output);
        module.Decorate(variable, spv::Decoration::Location, output.location);
        variables.push_back(variable);
        values.push_back(module.ConstantComposite(
            vec4_id, module.Constant(float_id, output.color[0]),
            module.Constant(float_id, output.color[1]), module.Constant(float_id, output.color[2]),
            module.Constant(float_id, output.color[3])));
    }
    interfaces = variables;
    const auto target = std::ranges::find(outputs, textured_location, &SolidOutput::location);
    if (target == outputs.end()) {
        textured = nullptr;
    }
    Sirit::Id input{}, image{}, sampler{}, image_type{}, sampler_type{};
    if (textured) {
        input = module.AddGlobalVariable(module.TypePointer(spv::StorageClass::Input, vec4_id),
                                         spv::StorageClass::Input);
        module.Decorate(input, spv::Decoration::Location, textured->location);
        interfaces.push_back(input);
        image_type = module.TypeImage(float_id, spv::Dim::Dim2D, 0, false, false, 1,
                                      spv::ImageFormat::Unknown);
        image = module.AddGlobalVariable(
            module.TypePointer(spv::StorageClass::UniformConstant, image_type),
            spv::StorageClass::UniformConstant);
        module.Decorate(image, spv::Decoration::DescriptorSet, 0U);
        module.Decorate(image, spv::Decoration::Binding, textured->image_binding);
        sampler_type = module.TypeSampler();
        sampler = module.AddGlobalVariable(
            module.TypePointer(spv::StorageClass::UniformConstant, sampler_type),
            spv::StorageClass::UniformConstant);
        module.Decorate(sampler, spv::Decoration::DescriptorSet, 0U);
        module.Decorate(sampler, spv::Decoration::Binding, textured->sampler_binding);
    }
    const auto main = module.OpFunction(void_id, spv::FunctionControlMask::MaskNone,
                                        module.TypeFunction(void_id));
    module.AddLabel();
    if (textured) {
        const auto interpolant = module.OpLoad(vec4_id, input);
        const auto uv = module.OpCompositeConstruct(
            vec2_id,
            module.OpCompositeExtract(float_id, interpolant, u32(textured->uv_comps[0])),
            module.OpCompositeExtract(float_id, interpolant, u32(textured->uv_comps[1])));
        const auto sampled_image =
            module.OpSampledImage(module.TypeSampledImage(image_type),
                                  module.OpLoad(image_type, image),
                                  module.OpLoad(sampler_type, sampler));
        const auto texel = module.OpImageSampleImplicitLod(vec4_id, sampled_image, uv);
        if (alpha_test) {
            const auto alpha = module.OpCompositeExtract(float_id, texel, 3u);
            const auto cut =
                module.OpFOrdLessThan(bool_id, alpha, module.Constant(float_id, 0.5f));
            const auto kill = module.OpLabel();
            const auto keep = module.OpLabel();
            module.OpSelectionMerge(keep, spv::SelectionControlMask::MaskNone);
            module.OpBranchConditional(cut, kill, keep);
            module.AddLabel(kill);
            module.OpKill();
            module.AddLabel(keep);
        }
        // The textured target shows the texture; its alpha stays opaque.
        values[target - outputs.begin()] = module.OpCompositeConstruct(
            vec4_id, module.OpCompositeExtract(float_id, texel, 0u),
            module.OpCompositeExtract(float_id, texel, 1u),
            module.OpCompositeExtract(float_id, texel, 2u), module.Constant(float_id, 1.0f));
    }
    for (size_t i = 0; i < variables.size(); ++i) {
        module.OpStore(variables[i], values[i]);
    }
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::Fragment, main, "main", interfaces);
    module.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    return module.Assemble();
}

} // namespace Shader::Backend::SPIRV
