#include <gencmd.hpp>
#include <device.hpp>
#include <layer.hpp>
#include <wrap.hpp>
#include <pipe.hpp>
#include <ds.hpp>
#include <threadContext.hpp>
#include <util/chain.hpp>

namespace vil {

// ugly, should be integrated into util/chain.hpp functions
void patchIndirectExecutionChain(LinAllocator& alloc, Device& dev, void* pNext) {
	auto next = static_cast<VkBaseOutStructure*>(pNext);
	while(next) {
		if(next->sType == VK_STRUCTURE_TYPE_GENERATED_COMMANDS_SHADER_INFO_EXT) {
			auto* shaders = reinterpret_cast<VkGeneratedCommandsShaderInfoEXT*>(next);
			auto shadersCopy = alloc.copy(shaders->pShaders, shaders->shaderCount);
			for(auto& shader : shadersCopy) {
				if(shader) {
					shader = get(dev, shader).handle;
				}
			}

			shaders->pShaders = shadersCopy.data();
		} else if(next->sType == VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT) {
			auto* pipe = reinterpret_cast<VkGeneratedCommandsPipelineInfoEXT*>(next);
			if(pipe->pipeline) {
				pipe->pipeline = get(dev, pipe->pipeline).handle;
			}
		}

		next = next->pNext;
	}
}

IndirectCommandsLayout::~IndirectCommandsLayout() = default;

VKAPI_ATTR void VKAPI_CALL GetGeneratedCommandsMemoryRequirementsEXT(
		VkDevice                                    device,
		const VkGeneratedCommandsMemoryRequirementsInfoEXT* pInfo,
		VkMemoryRequirements2*                      pMemoryRequirements) {
	auto& dev = getDevice(device);

	auto info = *pInfo;
	if (info.indirectCommandsLayout) {
		info.indirectCommandsLayout = get(device, pInfo->indirectCommandsLayout).handle;
	}
	if (info.indirectExecutionSet) {
		info.indirectExecutionSet = get(device, pInfo->indirectExecutionSet).handle;
	}

	// unwrap pipe/shader handles
	ThreadMemScope tms;
	auto copied = copyChainLocal(tms, info.pNext);
	info.pNext = copied;
	patchIndirectExecutionChain(tms.customUse(), dev, copied);

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

	PipelineLayout* pipeLayout {};
	if (nci.pipelineLayout) {
		pipeLayout = &get(device, nci.pipelineLayout);
		nci.pipelineLayout = pipeLayout->handle;
	}

	auto res = dev.dispatch.CreateIndirectCommandsLayoutEXT(dev.handle,
		&nci, pAllocator, pIndirectCommandsLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto cmdLayoutPtr = IntrusivePtr<IndirectCommandsLayout>(new IndirectCommandsLayout());
	auto& cmdLayout = *cmdLayoutPtr;
	cmdLayout.dev = &dev;
	cmdLayout.pipeLayout.reset(pipeLayout);
	cmdLayout.handle = *pIndirectCommandsLayout;
	cmdLayout.tokens = {pCreateInfo->pTokens, pCreateInfo->pTokens + pCreateInfo->tokenCount};
	cmdLayout.stride = pCreateInfo->indirectStride;
	cmdLayout.flags = pCreateInfo->flags;

	*pIndirectCommandsLayout = castDispatch<VkIndirectCommandsLayoutEXT>(cmdLayout);
	dev.indirectCommandsLayouts.mustEmplace(*pIndirectCommandsLayout, std::move(cmdLayoutPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyIndirectCommandsLayoutEXT(
		VkDevice                                    device,
		VkIndirectCommandsLayoutEXT                 indirectCommandsLayout,
		const VkAllocationCallbacks*                pAllocator) {
	auto cmdLayoutPtr = mustMoveUnset(device, indirectCommandsLayout);
	cmdLayoutPtr->dev->dispatch.DestroyIndirectCommandsLayoutEXT(
		cmdLayoutPtr->dev->handle, indirectCommandsLayout, pAllocator);
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

		span<VkIndirectExecutionSetShaderLayoutInfoEXT> layoutInfos;
		if (shaderInfo.pSetLayoutInfos) {
			layoutInfos = tms.copy(shaderInfo.pSetLayoutInfos, shaderInfo.maxShaderCount);
		}

		for(auto i = 0u; i < shaderInfo.maxShaderCount; ++i) {
			shaders[i] = get(device, shaders[i]).handle;

			if (!layoutInfos.empty()) {
				auto dsLayouts = tms.copy(layoutInfos[i].pSetLayouts, layoutInfos[i].setLayoutCount);
				for (auto& dsLayout : dsLayouts) {
					dsLayout = get(device, dsLayout).handle;
				}
				layoutInfos[i].pSetLayouts = dsLayouts.data();
			}
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

	auto exeSetPtr = IntrusivePtr<IndirectExecutionSet>(new IndirectExecutionSet());
	auto& exeSet = *exeSetPtr;
	exeSet.dev = &dev;
	exeSet.handle = *pIndirectExecutionSet;
	// TODO: remember unwrapped creation info

	*pIndirectExecutionSet = castDispatch<VkIndirectExecutionSetEXT>(exeSet);
	dev.indirectExecutionSets.mustEmplace(*pIndirectExecutionSet, std::move(exeSetPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyIndirectExecutionSetEXT(
		VkDevice                                    device,
		VkIndirectExecutionSetEXT                   indirectExecutionSet,
		const VkAllocationCallbacks*                pAllocator) {
	auto cmdLayoutPtr = mustMoveUnset(device, indirectExecutionSet);
	cmdLayoutPtr->dev->dispatch.DestroyIndirectExecutionSetEXT(
		cmdLayoutPtr->dev->handle, indirectExecutionSet, pAllocator);
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
