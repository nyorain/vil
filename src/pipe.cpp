#include "pipe.hpp"
#include "rp.hpp"
#include "ds.hpp"
#include "data.hpp"

namespace fuen {

PipelineShaderStage::PipelineShaderStage(Device& dev, const VkPipelineShaderStageCreateInfo& sci) {
	stage = sci.stage;

	if(auto* spec = sci.pSpecializationInfo) {
		auto data = static_cast<const std::byte*>(spec->pData);
		specData = {data, data + spec->dataSize};
		specEntries = {spec->pMapEntries, spec->pMapEntries + spec->mapEntryCount};
	}

	auto& mod = dev.shaderModules.get(sci.module);
	spirv = mod.code;
	entryPoint = sci.pName;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
		VkDevice                                    device,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkGraphicsPipelineCreateInfo*         pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateGraphicsPipelines(device, pipelineCache,
		createInfoCount, pCreateInfos, pAllocator, pPipelines);
	if(res != VK_SUCCESS) {
		return res;
	}

	for(auto i = 0u; i < createInfoCount; ++i) {
		dlg_assert(pPipelines[i]);
		auto& pci = pCreateInfos[i];

		auto& pipe = dev.graphicsPipes.add(pPipelines[i]);
		pipe.dev = &dev;
		pipe.objectType = VK_OBJECT_TYPE_PIPELINE;
		pipe.type = VK_PIPELINE_BIND_POINT_GRAPHICS;
		pipe.handle = pPipelines[i];
		pipe.layout = &dev.pipeLayouts.get(pci.layout);
		pipe.renderPass = dev.renderPasses.get(pci.renderPass).desc;
		pipe.subpass = pci.subpass;

		auto& subpassInfo = pipe.renderPass->subpasses[pipe.subpass];

		pipe.hasTessellation = false;
		pipe.hasMeshShader = false;
		pipe.hasDepthStencil = false;

		for(auto s = 0u; s < pci.stageCount; ++s) {
			pipe.stages.emplace_back(dev, pci.pStages[s]);
			dlg_assert((pci.pStages[s].stage & VK_SHADER_STAGE_ALL_GRAPHICS) != 0);

			// vulkan spec requires that if one tessellation stage is present,
			// both must be present.
			pipe.hasTessellation |= (pci.pStages[s].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
			pipe.hasMeshShader |= (pci.pStages[s].stage == VK_SHADER_STAGE_MESH_BIT_NV);
		}

		pipe.rasterizationState = *pci.pRasterizationState;

		if(!pipe.hasMeshShader) {
			pipe.vertexAttribs = {
				pci.pVertexInputState->pVertexAttributeDescriptions,
				pci.pVertexInputState->pVertexAttributeDescriptions + pci.pVertexInputState->vertexAttributeDescriptionCount
			};
			pipe.vertexBindings = {
				pci.pVertexInputState->pVertexBindingDescriptions,
				pci.pVertexInputState->pVertexBindingDescriptions + pci.pVertexInputState->vertexBindingDescriptionCount
			};

			pipe.vertexInputState = *pci.pVertexInputState;
			pipe.vertexInputState.pVertexAttributeDescriptions = pipe.vertexAttribs.data();
			pipe.vertexInputState.pVertexBindingDescriptions = pipe.vertexBindings.data();

			pipe.inputAssemblyState = *pci.pInputAssemblyState;
		}

		if(pci.pDynamicState) {
			pipe.dynamicState = {
				pci.pDynamicState->pDynamicStates,
				pci.pDynamicState->pDynamicStates + pci.pDynamicState->dynamicStateCount};
		}

		if(pci.pRasterizationState->rasterizerDiscardEnable) {
			pipe.multisampleState = *pci.pMultisampleState;
			pipe.viewportState = *pci.pViewportState;

			if(!pipe.dynamicState.count(VK_DYNAMIC_STATE_SCISSOR)) {
				pipe.scissors = {
					pipe.viewportState.pScissors,
					pipe.viewportState.pScissors + pipe.viewportState.scissorCount
				};
				pipe.viewportState.pScissors = pipe.scissors.data();
			}

			if(!pipe.dynamicState.count(VK_DYNAMIC_STATE_VIEWPORT)) {
				pipe.viewports = {
					pipe.viewportState.pViewports,
					pipe.viewportState.pViewports + pipe.viewportState.viewportCount
				};
				pipe.viewportState.pViewports = pipe.viewports.data();
			}

			pipe.hasDepthStencil = subpassInfo.pDepthStencilAttachment;
			if(pipe.hasDepthStencil) {
				pipe.depthStencilState = *pci.pDepthStencilState;
			}
		}

		if(pipe.hasTessellation) {
			dlg_assert(pci.pTessellationState);
			pipe.tessellationState = *pci.pTessellationState;
		}

		if(subpassInfo.colorAttachmentCount) {
			pipe.blendAttachments = {
				pci.pColorBlendState->pAttachments,
				pci.pColorBlendState->pAttachments + pci.pColorBlendState->attachmentCount
			};

			pipe.colorBlendState = *pci.pColorBlendState;
			pipe.colorBlendState.pAttachments = pipe.blendAttachments.data();
		}
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
		VkDevice                                    device,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkComputePipelineCreateInfo*          pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateComputePipelines(device, pipelineCache,
		createInfoCount, pCreateInfos, pAllocator, pPipelines);
	if(res != VK_SUCCESS) {
		return res;
	}

	for(auto i = 0u; i < createInfoCount; ++i) {
		dlg_assert(pPipelines[i]);

		auto& pipe = dev.computePipes.add(pPipelines[i]);
		pipe.objectType = VK_OBJECT_TYPE_PIPELINE;
		pipe.type = VK_PIPELINE_BIND_POINT_COMPUTE;
		pipe.dev = &dev;
		pipe.handle = pPipelines[i];
		pipe.layout = &dev.pipeLayouts.get(pCreateInfos[i].layout);
		pipe.stage = PipelineShaderStage(dev, pCreateInfos[i].stage);
		dlg_assert(pipe.stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);

	auto count = dev.graphicsPipes.erase(pipeline) + dev.computePipes.erase(pipeline);
	dlg_assert(count == 1);

	dev.dispatch.DestroyPipeline(device, pipeline, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(
		VkDevice                                    device,
		const VkPipelineLayoutCreateInfo*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkPipelineLayout*                           pPipelineLayout) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreatePipelineLayout(device, pCreateInfo,
		pAllocator, pPipelineLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& pl = dev.pipeLayouts.add(*pPipelineLayout);
	pl.objectType = VK_OBJECT_TYPE_PIPELINE_LAYOUT;
	pl.dev = &dev;
	pl.handle = *pPipelineLayout;

	for(auto i = 0u; i < pCreateInfo->setLayoutCount; ++i) {
		pl.descriptors.push_back(&dev.dsLayouts.get(pCreateInfo->pSetLayouts[i]));
	}

	for(auto i = 0u; i < pCreateInfo->pushConstantRangeCount; ++i) {
		pl.pushConstants.push_back(pCreateInfo->pPushConstantRanges[i]);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(
		VkDevice                                    device,
		VkPipelineLayout                            pipelineLayout,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.pipeLayouts.mustErase(pipelineLayout);
	dev.dispatch.DestroyPipelineLayout(device, pipelineLayout, pAllocator);
}

bool pushConstantCompatible(const PipelineLayout& a, const PipelineLayout& b) {
	// Difficulty here: a pipeline layout created with one push constant
	// range (0, 4) for vertex and fragment stage is (per my understanding
	// of the spec) compatible with one that has two push constants ranges
	// (0, 4) for vertex and fragment stage each.
	// We therefore simply check two things for compatibility:
	// - For each range pcra declared in a, any range pcrb in b that shares any
	//   stages with pcra must have same offset and size. Furthermore
	//   for all stages in pcra there must an range in pcrb (with same offset
	//   and size) that contains this stage.
	// - vice versa
	auto check = [](auto& a, auto& b) {
		for(auto& pcra : a.pushConstants) {
			auto accum = i32{};
			for(auto& pcrb : b.pushConstants) {
				auto intersection = pcra.stageFlags & pcrb.stageFlags;
				if(intersection == 0) {
					continue;
				}

				accum |= intersection;
				if(pcra.offset != pcrb.offset || pcra.size != pcrb.size) {
					return false;
				}
			}

			if(accum != i32(pcra.stageFlags)) {
				return false;
			}
		}

		return true;
	};

	return check(a, b) && check(b, a);
}

bool compatibleForSetN(const PipelineLayout& a, const PipelineLayout& b, u32 N) {
	if(!pushConstantCompatible(a, b)) {
		return false;
	}

	if(a.descriptors.size() <= N || b.descriptors.size() <= N) {
		return false;
	}

	for(auto s = 0u; s < N; ++s) {
		auto& da = *a.descriptors[s];
		auto& db = *b.descriptors[s];
		if(da.bindings.size() != db.bindings.size()) {
			return false;
		}

		// bindings are sorted by binding number so we can simply compare
		// them in order
		for(auto b = 0u; b < da.bindings.size(); ++b) {
			auto& ba = da.bindings[b];
			auto& bb = db.bindings[b];

			if(ba.binding != bb.binding ||
					ba.descriptorCount != bb.descriptorCount ||
					ba.descriptorType != bb.descriptorType ||
					ba.stageFlags != bb.stageFlags) {
				return false;
			}

			// immutable samplers
			if(ba.binding == VK_DESCRIPTOR_TYPE_SAMPLER ||
					ba.binding == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
				if(bool(ba.pImmutableSamplers) != bool(bb.pImmutableSamplers)) {
					return false;
				}

				if(ba.pImmutableSamplers) {
					for(auto e = 0u; e < ba.descriptorCount; ++e) {
						if(ba.pImmutableSamplers[e] != bb.pImmutableSamplers[e]) {
							return false;
						}
					}
				}
			}
		}
	}

	return true;
}

} // namespace fuen
