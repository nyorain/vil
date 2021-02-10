#include <pipe.hpp>
#include <device.hpp>
#include <rp.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <data.hpp>

namespace vil {

PipelineLayout::~PipelineLayout() {
	if(!dev) {
		return;
	}

	// pipe layouts are never used directly by command buffers.
	dlg_assert(refRecords.empty());
	dlg_assert(handle);

	dev->dispatch.DestroyPipelineLayout(dev->handle, handle, nullptr);
}

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
		pipe.layout = dev.pipeLayouts.getPtr(pci.layout);
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

		if(!pci.pRasterizationState->rasterizerDiscardEnable) {
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
		pipe.layout = dev.pipeLayouts.getPtr(pCreateInfos[i].layout);
		pipe.stage = PipelineShaderStage(dev, pCreateInfos[i].stage);
		dlg_assert(pipe.stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		const VkAllocationCallbacks*                pAllocator) {
	if(!pipeline) {
		return;
	}

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
	// NOTE: we don't use host allocators here since this handle is potentially
	// kept alive inside the layer, preventing us from passing an application
	// allocator to the destruction function.
	// See design.md on allocators.
	(void) pAllocator;

	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreatePipelineLayout(device, pCreateInfo, nullptr, pPipelineLayout);
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
	if(!pipelineLayout) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.pipeLayouts.mustErase(pipelineLayout);

	// NOTE: We intenntionally don't destruct the handle here, handle might
	// need to be kept alive, they have shared ownership. Destroyed
	// in handle destructor.
	// dev.dispatch.DestroyPipelineLayout(device, pipelineLayout, pAllocator);
	(void) pAllocator;
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

bool compatibleForSetN(const PipelineLayout& pl1, const PipelineLayout& pl2, u32 N) {
	if(!pushConstantCompatible(pl1, pl2)) {
		// dlg_trace("!compatible({}): push constants", N);
		return false;
	}

	if(pl1.descriptors.size() <= N || pl2.descriptors.size() <= N) {
		// dlg_trace("!compatible({}): sizes ({}, {})", N, pl1.descriptors.size(), pl2.descriptors.size());
		return false;
	}

	for(auto s = 0u; s <= N; ++s) {
		auto& da = *pl1.descriptors[s];
		auto& db = *pl2.descriptors[s];
		if(&da == &db) {
			continue; // layout always compatible with itself
		}

		if(da.bindings.size() != db.bindings.size()) {
			// dlg_trace("!compatible({}): binding count {} vs {} (set {})", N,
			// 	da.bindings.size(), db.bindings.size(), s);
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
				// dlg_trace("!compatible({}): binding {} (set {})", N, b, s);
				return false;
			}

			// immutable samplers
			if(ba.binding == VK_DESCRIPTOR_TYPE_SAMPLER ||
					ba.binding == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
				if(bool(ba.immutableSamplers) != bool(bb.immutableSamplers)) {
					// dlg_trace("!compatible({}): immutable samplers, binding {} (set {})", N, b, s);
					return false;
				}

				if(ba.immutableSamplers) {
					dlg_assert(ba.descriptorCount == bb.descriptorCount);
					for(auto e = 0u; e < ba.descriptorCount; ++e) {
						// TODO: what if the samplers are compatible?
						if(ba.immutableSamplers[e] != bb.immutableSamplers[e]) {
							// dlg_trace("!compatible({}): immutable samplers(2), binding {} (set {})", N, b, s);
							return false;
						}
					}
				}
			}
		}
	}

	return true;
}

} // namespace vil
