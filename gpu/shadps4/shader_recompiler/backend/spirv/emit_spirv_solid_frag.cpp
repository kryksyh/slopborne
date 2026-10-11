// SPDX-License-Identifier: GPL-2.0-or-later

#include <sirit/sirit.h>

#include "shader_recompiler/backend/spirv/emit_spirv_solid_frag.h"

namespace Shader::Backend::SPIRV {

std::vector<u32> EmitSolidFragmentShader(std::span<const SolidOutput> outputs) {
    Sirit::Module module{0x00010300};
    module.AddCapability(spv::Capability::Shader);
    module.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const auto void_id = module.TypeVoid();
    const auto float_id = module.TypeFloat(32);
    const auto vec4_id = module.TypeVector(float_id, 4);
    const auto output_ptr = module.TypePointer(spv::StorageClass::Output, vec4_id);
    std::vector<Sirit::Id> variables, values;
    for (const auto& output : outputs) {
        const auto variable = module.AddGlobalVariable(output_ptr, spv::StorageClass::Output);
        module.Decorate(variable, spv::Decoration::Location, output.location);
        variables.push_back(variable);
        values.push_back(module.ConstantComposite(
            vec4_id, module.Constant(float_id, output.color[0]),
            module.Constant(float_id, output.color[1]), module.Constant(float_id, output.color[2]),
            module.Constant(float_id, output.color[3])));
    }
    const auto main = module.OpFunction(void_id, spv::FunctionControlMask::MaskNone,
                                        module.TypeFunction(void_id));
    module.AddLabel();
    for (size_t i = 0; i < variables.size(); ++i) {
        module.OpStore(variables[i], values[i]);
    }
    module.OpReturn();
    module.OpFunctionEnd();
    module.AddEntryPoint(spv::ExecutionModel::Fragment, main, "main", variables);
    module.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    return module.Assemble();
}

} // namespace Shader::Backend::SPIRV
