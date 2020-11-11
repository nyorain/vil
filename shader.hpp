#pragma once

#include "common.hpp"
#include <memory>

typedef struct SpvReflectShaderModule SpvReflectShaderModule;

namespace fuen {

struct ShaderModule {
	Device* dev;
	VkShaderModule handle;
	std::vector<u32> spv;
	std::unique_ptr<SpvReflectShaderModule> reflection;

	~ShaderModule();
};

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice                                    device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderModule*                             pShaderModule);

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
    VkDevice                                    device,
    VkShaderModule                              shaderModule,
    const VkAllocationCallbacks*                pAllocator);

} // namespace fuen
