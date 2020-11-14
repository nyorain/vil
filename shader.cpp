#include "shader.hpp"
#include "data.hpp"

#include "spirv_reflect.h"

namespace fuen {

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
	auto res = dev.dispatch.vkCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& mod = dev.shaderModules.add(*pShaderModule);
	mod.dev = &dev;
	mod.handle = *pShaderModule;

	mod.code = std::make_shared<SpirvData>();
	mod.code->spv = {pCreateInfo->pCode, pCreateInfo->pCode + pCreateInfo->codeSize};
	mod.code->reflection = std::make_unique<SpvReflectShaderModule>();
	auto reflRes = spvReflectCreateShaderModule(pCreateInfo->codeSize,
		pCreateInfo->pCode, mod.code->reflection.get());
	dlg_assert(reflRes == SPV_REFLECT_RESULT_SUCCESS);

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
		VkDevice                                    device,
		VkShaderModule                              shaderModule,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.shaderModules.mustErase(shaderModule);
	dev.dispatch.vkDestroyShaderModule(device, shaderModule, pAllocator);
}

} // namespace fuen
