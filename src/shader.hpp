#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <vk/vulkan.h>

#include <memory>
#include <vector>

namespace vil {

typedef struct SpvReflectShaderModule SpvReflectShaderModule;

struct SpirvData {
	std::vector<u32> spv;
	std::unique_ptr<SpvReflectShaderModule> reflection;

	~SpirvData();
};

struct ShaderModule : DeviceHandle {
	VkShaderModule handle {};

	// Managed via shared ptr since it may outlive the shader module in
	// (possibly multiple) pipeline objects.
	std::shared_ptr<SpirvData> code;

	// Owend by us.
	// When the shader module is a vertex shader, we lazily create
	// a transform-feedback version of it.
	VkShaderModule xfbVertShader {};
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

} // namespace vil
