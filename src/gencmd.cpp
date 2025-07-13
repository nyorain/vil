#include <gencmd.hpp>
#include <device.hpp>
#include <layer.hpp>
#include <wrap.hpp>
#include <pipe.hpp>
#include <ds.hpp>
#include <threadContext.hpp>

namespace vil {

IndirectCommandsLayout::~IndirectCommandsLayout() = default;

VKAPI_ATTR void VKAPI_CALL GetGeneratedCommandsMemoryRequirementsEXT(
		VkDevice                                    device,
		const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo,
		VkMemoryRequirements2*                      pMemoryRequirements) {
	auto& dev = getDevice(device);

	auto info = *pInfo;
	info.indirectCommandsLayout = get(device, pInfo->indirectCommandsLayout).handle;
	info.indirectExecutionSet = get(device, pInfo->indirectExecutionSet).handle;

	return dev.dispatch.GetGeneratedCommandsMemoryRequirementsEXT(device,
		&info, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateIndirectCommandsLayoutEXT(
		VkDevice                                    device,
		const VkIndirectCommandsLayoutCreateInfoEXT* pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkIndirectCommandsLayoutEXT*                pIndirectCommandsLayout) {
	auto& dev = getDevice(device);
	auto nci = *pCreateInfo;
	auto& pipeLayout = get(device, nci.pipelineLayout);
	nci.pipelineLayout = pipeLayout.handle;

	auto res = dev.dispatch.CreateIndirectCommandsLayoutEXT(dev.handle,
		&nci, pAllocator, pIndirectCommandsLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& cmdLayout = dev.indirectCommandsLayouts.add(*pIndirectCommandsLayout);
	cmdLayout.dev = &dev;
	cmdLayout.pipeLayout.reset(&pipeLayout);
	cmdLayout.handle = *pIndirectCommandsLayout;
	cmdLayout.tokens = {pCreateInfo->pTokens, pCreateInfo->pTokens + pCreateInfo->tokenCount};
	cmdLayout.stride = pCreateInfo->indirectStride;
	cmdLayout.flags = pCreateInfo->flags;
	*pIndirectCommandsLayout = castDispatch<VkIndirectCommandsLayoutEXT>(cmdLayout);

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyIndirectCommandsLayoutEXT(
		VkDevice                                    device,
		VkIndirectCommandsLayoutEXT                 indirectCommandsLayout,
		const VkAllocationCallbacks*                pAllocator) {
	auto cmdLayoutPtr = mustMoveUnset(device, indirectCommandsLayout);
	cmdLayoutPtr->dev->dispatch.DestroyIndirectCommandsLayoutEXT(
		cmdLayoutPtr->dev->handle, cmdLayoutPtr->handle, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateIndirectExecutionSetEXT(
		VkDevice                                    device,
		const VkIndirectExecutionSetCreateInfoEXT*  pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkIndirectExecutionSetEXT*                  pIndirectExecutionSet) {
	auto& dev = getDevice(device);
	auto nci = *pCreateInfo;

	ThreadMemScope tms;
	VkIndirectExecutionSetPipelineInfoEXT pipeInfo;
	VkIndirectExecutionSetShaderInfoEXT shaderInfo;

	if (nci.type == VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT) {
		pipeInfo = *nci.info.pPipelineInfo;
		nci.info.pPipelineInfo = &pipeInfo;

		pipeInfo.initialPipeline = get(device, pipeInfo.initialPipeline).handle;
	} else if (nci.type == VK_INDIRECT_EXECUTION_SET_INFO_TYPE_SHADER_OBJECTS_EXT) {
		shaderInfo = *nci.info.pShaderInfo;
		nci.info.pShaderInfo = &shaderInfo;

		auto shaders = tms.copy(shaderInfo.pInitialShaders, shaderInfo.maxShaderCount);
		auto layoutInfos = tms.copy(shaderInfo.pSetLayoutInfos, shaderInfo.maxShaderCount);

		for(auto i = 0u; i < shaderInfo.maxShaderCount; ++i) {
			shaders[i] = get(device, shaders[i]).handle;

			auto dsLayouts = tms.copy(layoutInfos[i].pSetLayouts, layoutInfos[i].setLayoutCount);
			for (auto& dsLayout : dsLayouts) {
				dsLayout = get(device, dsLayout).handle;
			}
			layoutInfos[i].pSetLayouts = dsLayouts.data();
		}

		shaderInfo.pInitialShaders = shaders.data();
		shaderInfo.pSetLayoutInfos = layoutInfos.data();
	} else {
		dlg_error("unexpected");
	}

	auto res = dev.dispatch.CreateIndirectExecutionSetEXT(dev.handle,
		&nci, pAllocator, pIndirectExecutionSet);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& cmdLayout = dev.indirectExecutionSets.add(*pIndirectExecutionSet);
	cmdLayout.dev = &dev;
	// TODO: remember unwrapped creation info
	cmdLayout.handle = *pIndirectExecutionSet;
	*pIndirectExecutionSet = castDispatch<VkIndirectExecutionSetEXT>(cmdLayout);

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyIndirectExecutionSetEXT(
		VkDevice                                    device,
		VkIndirectExecutionSetEXT                   indirectExecutionSet,
		const VkAllocationCallbacks*                pAllocator) {
	auto cmdLayoutPtr = mustMoveUnset(device, indirectExecutionSet);
	cmdLayoutPtr->dev->dispatch.DestroyIndirectExecutionSetEXT(
		cmdLayoutPtr->dev->handle, cmdLayoutPtr->handle, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL UpdateIndirectExecutionSetPipelineEXT(
		VkDevice                                    device,
		VkIndirectExecutionSetEXT                   indirectExecutionSet,
		uint32_t                                    executionSetWriteCount,
		const VkWriteIndirectExecutionSetPipelineEXT* pExecutionSetWrites) {
	auto& execSet = get(device, indirectExecutionSet);
	auto& dev = *execSet.dev;

	ThreadMemScope tms;
	auto writes = tms.copy(pExecutionSetWrites, executionSetWriteCount);
	for(auto& write : writes) {
		write.pipeline = get(device, write.pipeline).handle;
	}

	dev.dispatch.UpdateIndirectExecutionSetPipelineEXT(dev.handle,
		execSet.handle, writes.size(), writes.data());
}

VKAPI_ATTR void VKAPI_CALL UpdateIndirectExecutionSetShaderEXT(
		VkDevice                                    device,
		VkIndirectExecutionSetEXT                   indirectExecutionSet,
		uint32_t                                    executionSetWriteCount,
		const VkWriteIndirectExecutionSetShaderEXT* pExecutionSetWrites) {
	auto& execSet = get(device, indirectExecutionSet);
	auto& dev = *execSet.dev;

	ThreadMemScope tms;
	auto writes = tms.copy(pExecutionSetWrites, executionSetWriteCount);
	for(auto& write : writes) {
		write.shader = get(device, write.shader).handle;
	}

	dev.dispatch.UpdateIndirectExecutionSetShaderEXT(dev.handle,
		execSet.handle, writes.size(), writes.data());
}

} // namespace
