#include <shader.hpp>
#include <device.hpp>
#include <data.hpp>

#include "spirv_reflect.h"

namespace vil {

// ShaderModule
SpirvData::~SpirvData() {
	spvReflectDestroyShaderModule(reflection.get());
}

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
		VkDevice                                    device,
		const VkShaderModuleCreateInfo*             pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkShaderModule*                             pShaderModule) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& mod = dev.shaderModules.add(*pShaderModule);
	mod.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
	mod.dev = &dev;
	mod.handle = *pShaderModule;

	mod.code = std::make_shared<SpirvData>();
	dlg_assert(pCreateInfo->codeSize % 4 == 0);
	mod.code->spv = {pCreateInfo->pCode, pCreateInfo->pCode + pCreateInfo->codeSize / 4};
	mod.code->reflection = std::make_unique<SpvReflectShaderModule>();
	auto reflRes = spvReflectCreateShaderModule(pCreateInfo->codeSize,
		pCreateInfo->pCode, mod.code->reflection.get());
	dlg_assertl(dlg_level_info, reflRes == SPV_REFLECT_RESULT_SUCCESS);

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
		VkDevice                                    device,
		VkShaderModule                              shaderModule,
		const VkAllocationCallbacks*                pAllocator) {
	if (!shaderModule) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.shaderModules.mustErase(shaderModule);
	dev.dispatch.DestroyShaderModule(device, shaderModule, pAllocator);
}

} // namespace vil
