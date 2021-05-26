#include <pipe.hpp>
#include <device.hpp>
#include <rp.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <data.hpp>
#include <threadContext.hpp>
#include <util/spirv.hpp>
#include <util/util.hpp>
#include <dlg/dlg.hpp>
#include <vk/enumString.hpp>

namespace vil {

// PipelineLayout
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

// API
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
		VkDevice                                    device,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkGraphicsPipelineCreateInfo*         pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	ZoneScoped;
	auto& dev = getDevice(device);

	// We can't create the pipelines one-by-one since that would mess
	// with the derivation fields.

	struct PreData {
		IntrusivePtr<XfbPatchDesc> xfb {};
		span<const VkPipelineShaderStageCreateInfo> stages;
	};

	auto ncis = LocalVector<VkGraphicsPipelineCreateInfo>(createInfoCount);
	auto pres = LocalVector<PreData>(createInfoCount);
	std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stagesVecs;

	for(auto i = 0u; i < createInfoCount; ++i) {
		auto& nci = ncis[i];
		nci = pCreateInfos[i];
		nci.layout = get(dev, nci.layout).handle;

		auto& pre = pres[i];

		auto& rp = dev.renderPasses.get(nci.renderPass);
		pre.stages = {nci.pStages, nci.stageCount};
		u32 xfbVertexStageID = u32(-1);

		// transform feedback isn't supported for multiview graphics pipelines
		auto useXfb = dev.transformFeedback &&
			!hasChain(*rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);

		auto& stages = stagesVecs.emplace_back();
		for(auto s = 0u; s < nci.stageCount; ++s) {
			auto src = nci.pStages[s];
			if(src.stage == VK_SHADER_STAGE_VERTEX_BIT && useXfb) {
				dlg_assert(xfbVertexStageID == u32(-1));
				xfbVertexStageID = u32(stages.size());
			}

			if(src.stage == VK_SHADER_STAGE_GEOMETRY_BIT ||
					src.stage == VK_SHADER_STAGE_MESH_BIT_NV ||
					src.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
					src.stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
				// Builtin Position might not be present
				dlg_trace("xfb currently not supported for mesh/geometry/tessellation pipes");
				useXfb = false;
			}

			stages.push_back(src);
		}

		useXfb &= xfbVertexStageID != u32(-1);
		if (useXfb) {
			auto& stage = stages[xfbVertexStageID];
			auto& mod = dev.shaderModules.get(stage.module);
			auto spec = createShaderSpecialization(stage.pSpecializationInfo);

			{
				// All we are doing here is basically lazily creating
				// an xfb-patched version of the vertex shader. We want to
				// avoid locking dev.mutex *while* we actually patch it since
				// we have to iterate over the module for that (not cheap for
				// huge modules).
				std::unique_lock lock(dev.mutex);

				auto finder = [&](const XfbPatchData& patched) {
					return patched.entryPoint == stage.pName && patched.spec == spec;
				};
				auto it = find_if(mod.xfb, finder);
				if(it == mod.xfb.end()) {
					lock.unlock();
					auto xfb = patchVertexShaderXfb(dev, mod.spv, stage.pName,
						std::move(spec), mod.name);
					lock.lock();

					// check again, it might have been constructed by another thread in the meantime
					it = find_if(mod.xfb, finder);
					if(xfb.mod) {
						if(it == mod.xfb.end()) {
							stage.module = xfb.mod;
							it = mod.xfb.emplace(it, std::move(xfb));
						} else if(xfb.mod) {
							dev.dispatch.DestroyShaderModule(dev.handle, xfb.mod, nullptr);
						}
					}
				}

				if(it != mod.xfb.end()) {
					stage.module = it->mod;
					pre.xfb = it->desc;
				}
			}
		}

		nci.pStages = stages.data();
	}

	{
		ZoneScopedN("dispatch");
		auto res = dev.dispatch.CreateGraphicsPipelines(dev.handle, pipelineCache,
			u32(ncis.size()), ncis.data(), pAllocator, pPipelines);
		if (res != VK_SUCCESS) {
			return res;
		}
	}

	for(auto i = 0u; i < createInfoCount; ++i) {
		dlg_assert(pPipelines[i]);
		auto& pci = ncis[i];

		auto& pipe = dev.graphicsPipes.add(pPipelines[i]);
		pipe.dev = &dev;
		pipe.objectType = VK_OBJECT_TYPE_PIPELINE;
		pipe.type = VK_PIPELINE_BIND_POINT_GRAPHICS;
		pipe.handle = pPipelines[i];
		pipe.layout = getPtr(dev, pCreateInfos[i].layout);
		pipe.renderPass = dev.renderPasses.get(pci.renderPass).desc;
		pipe.subpass = pci.subpass;

		pci.layout = pipe.layout->handle;

		auto& subpassInfo = pipe.renderPass->subpasses[pipe.subpass];

		pipe.hasTessellation = false;
		pipe.hasMeshShader = false;
		pipe.hasDepthStencil = false;
		pipe.xfbPatch = std::move(pres[i].xfb);

		for(auto stage : pres[i].stages) {
			pipe.stages.emplace_back(dev, stage);
			dlg_assert((stage.stage & VK_SHADER_STAGE_ALL_GRAPHICS) != 0);

			// vulkan spec requires that if one tessellation stage is present,
			// both must be present.
			pipe.hasTessellation |= (stage.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
			pipe.hasMeshShader |= (stage.stage == VK_SHADER_STAGE_MESH_BIT_NV);
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

	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
		VkDevice                                    device,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkComputePipelineCreateInfo*          pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	ZoneScoped;
	auto& dev = getDevice(device);

	auto ncis = LocalVector<VkComputePipelineCreateInfo>(createInfoCount);
	for(auto i = 0u; i < createInfoCount; ++i) {
		auto& nci = ncis[i];
		nci = pCreateInfos[i];
		nci.layout = get(device, nci.layout).handle;
	}

	{
		ZoneScopedN("dispatch");
		auto res = dev.dispatch.CreateComputePipelines(dev.handle, pipelineCache,
			createInfoCount, ncis.data(), pAllocator, pPipelines);
		if(res != VK_SUCCESS) {
			return res;
		}
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

	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		const VkAllocationCallbacks*                pAllocator) {
	if(!pipeline) {
		return;
	}

	auto& dev = getDevice(device);

	auto count = dev.graphicsPipes.erase(pipeline) + dev.computePipes.erase(pipeline);
	dlg_assert(count == 1);

	dev.dispatch.DestroyPipeline(dev.handle, pipeline, pAllocator);
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

	auto& dev = getDevice(device);

	auto dslHandles = LocalVector<VkDescriptorSetLayout>(pCreateInfo->setLayoutCount);
	auto dsls = std::vector<IntrusivePtr<DescriptorSetLayout>>(pCreateInfo->setLayoutCount);
	for(auto i = 0u; i < pCreateInfo->setLayoutCount; ++i) {
		dsls[i] = getPtr(dev, pCreateInfo->pSetLayouts[i]);
		dslHandles[i] = dsls[i]->handle;
	}

	auto nci = *pCreateInfo;
	nci.pSetLayouts = dslHandles.data();

	auto res = dev.dispatch.CreatePipelineLayout(dev.handle, &nci, nullptr, pPipelineLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto plPtr = IntrusivePtr<PipelineLayout>(new PipelineLayout());
	auto& pl = *plPtr;
	pl.objectType = VK_OBJECT_TYPE_PIPELINE_LAYOUT;
	pl.dev = &dev;
	pl.handle = *pPipelineLayout;
	pl.descriptors = std::move(dsls);

	for(auto i = 0u; i < pCreateInfo->pushConstantRangeCount; ++i) {
		pl.pushConstants.push_back(pCreateInfo->pPushConstantRanges[i]);
	}

	*pPipelineLayout = castDispatch<VkPipelineLayout>(pl);
	dev.pipeLayouts.mustEmplace(*pPipelineLayout, std::move(plPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(
		VkDevice                                    device,
		VkPipelineLayout                            pipelineLayout,
		const VkAllocationCallbacks*                pAllocator) {
	if(!pipelineLayout) {
		return;
	}

	auto& dev = getDevice(device);
	dev.pipeLayouts.mustErase(pipelineLayout);

	// NOTE: We intenntionally don't destruct the handle here, handle might
	// need to be kept alive, they have shared ownership. Destroyed
	// in handle destructor. See e.g. the pipeLayouts hash map in Device
	// for justification why we need this.
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

bool compatibleForSetN(const PipelineLayout& pl1, const PipelineLayout& pl2,
		u32 N, bool considerPushConstants) {
	if(considerPushConstants && !pushConstantCompatible(pl1, pl2)) {
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

		if(!compatible(da, db)) {
			return false;
		}
	}

	return true;
}

GraphicsPipeline::~GraphicsPipeline() = default;

} // namespace vil
