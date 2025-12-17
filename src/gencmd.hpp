#pragma once

#include <handle.hpp>
#include <util/intrusive.hpp>

namespace vil {

struct IndirectCommandsLayout : public SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT;

	IntrusivePtr<PipelineLayout> pipeLayout {};
	VkIndirectCommandsLayoutEXT handle {};
	std::vector<VkIndirectCommandsLayoutTokenEXT> tokens {};
	u32 stride {};
	VkIndirectCommandsLayoutUsageFlagsEXT flags {};

	~IndirectCommandsLayout();
};

struct IndirectExecutionSet : public SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT;

	VkIndirectExecutionSetEXT handle {};
};

void patchIndirectExecutionChain(LinAllocator& alloc, Device& dev, void* pNext);


// api
VKAPI_ATTR void VKAPI_CALL GetGeneratedCommandsMemoryRequirementsEXT(
    VkDevice                                    device,
    const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements);

VKAPI_ATTR VkResult VKAPI_CALL CreateIndirectCommandsLayoutEXT(
    VkDevice                                    device,
    const VkIndirectCommandsLayoutCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkIndirectCommandsLayoutEXT*                pIndirectCommandsLayout);

VKAPI_ATTR void VKAPI_CALL DestroyIndirectCommandsLayoutEXT(
    VkDevice                                    device,
    VkIndirectCommandsLayoutEXT                 indirectCommandsLayout,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL CreateIndirectExecutionSetEXT(
    VkDevice                                    device,
    const VkIndirectExecutionSetCreateInfoEXT*  pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkIndirectExecutionSetEXT*                  pIndirectExecutionSet);

VKAPI_ATTR void VKAPI_CALL DestroyIndirectExecutionSetEXT(
    VkDevice                                    device,
    VkIndirectExecutionSetEXT                   indirectExecutionSet,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR void VKAPI_CALL UpdateIndirectExecutionSetPipelineEXT(
    VkDevice                                    device,
    VkIndirectExecutionSetEXT                   indirectExecutionSet,
    uint32_t                                    executionSetWriteCount,
    const VkWriteIndirectExecutionSetPipelineEXT* pExecutionSetWrites);

VKAPI_ATTR void VKAPI_CALL UpdateIndirectExecutionSetShaderEXT(
    VkDevice                                    device,
    VkIndirectExecutionSetEXT                   indirectExecutionSet,
    uint32_t                                    executionSetWriteCount,
    const VkWriteIndirectExecutionSetShaderEXT* pExecutionSetWrites);

} // namespace
