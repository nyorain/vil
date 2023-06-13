#include <pipe.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <rp.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <spirv-cross/spirv.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <util/spirv.hpp>
#include <util/util.hpp>
#include <util/dlg.hpp>
#include <vkutil/enumString.hpp>

namespace vil {

// PipelineLayout
PipelineLayout::~PipelineLayout() {
	if(!dev) {
		return;
	}

	dlg_assert(handle);
	dev->dispatch.DestroyPipelineLayout(dev->handle, handle, nullptr);
}

PipelineShaderStage::PipelineShaderStage(Device& dev, const VkPipelineShaderStageCreateInfo& sci) {
	stage = sci.stage;
	specialization = createShaderSpecialization(sci.pSpecializationInfo);

	spirv = getPtr(dev, sci.module);
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
		IntrusivePtr<RenderPass> rp {};
		IntrusivePtr<XfbPatchDesc> xfb {};
		span<const VkPipelineShaderStageCreateInfo> stages;
	};

	ThreadMemScope memScope;
	auto ncis = memScope.copy(pCreateInfos, createInfoCount);

	std::vector<PreData> pres;
	pres.resize(createInfoCount);

	// TODO: use memScope as well
	std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stagesVecs;

	for(auto i = 0u; i < createInfoCount; ++i) {
		auto& nci = ncis[i];
		nci.layout = get(dev, nci.layout).handle;

		if(nci.basePipelineHandle) {
			auto& basePipe = get(dev, nci.basePipelineHandle);
			nci.basePipelineHandle = basePipe.handle;
		}

		auto& pre = pres[i];

		if(nci.renderPass) {
			pre.rp = getPtr(dev, nci.renderPass);
		}

		pre.stages = {nci.pStages, nci.stageCount};
		u32 xfbVertexStageID = u32(-1);

		// transform feedback isn't supported for multiview graphics pipelines
		// TODO: support it for non-multiview dynamic rendering
		auto useXfb = dev.transformFeedback &&
			pre.rp &&
			!hasChain(pre.rp->desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);

		auto& stages = stagesVecs.emplace_back();
		for(auto s = 0u; s < nci.stageCount; ++s) {
			auto src = nci.pStages[s];
			if(src.stage == VK_SHADER_STAGE_VERTEX_BIT && useXfb) {
				dlg_assert(xfbVertexStageID == u32(-1));
				xfbVertexStageID = u32(stages.size());
			}

			if(src.stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ||
					src.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
					src.stage == VK_SHADER_STAGE_MESH_BIT_NV) {
				useXfb = false;
				xfbVertexStageID = u32(-1);
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
				auto itValid = true;
				if(it == mod.xfb.end()) {
					itValid = false;
					lock.unlock();
					XfbPatchData xfb;

					{
						auto& refl = specializeSpirv(mod, spec,
							stage.pName, u32(spv::ExecutionModelVertex));
						xfb = patchShaderXfb(dev, refl, stage.pName, mod.name);
					}

					if(xfb.mod) {
						lock.lock();
						// check again, it might have been constructed by another thread in the meantime
						it = find_if(mod.xfb, finder);

						if(it == mod.xfb.end()) {
							xfb.spec = std::move(spec);
							stage.module = xfb.mod;
							it = mod.xfb.emplace(it, std::move(xfb));
						} else if(xfb.mod) {
							dlg_trace("xfb patch race prevented");
							dev.dispatch.DestroyShaderModule(dev.handle, xfb.mod, nullptr);
						}

						itValid = true;
					}
				}

				if(itValid) {
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

		auto pipePtr = IntrusivePtr<GraphicsPipeline>(new GraphicsPipeline());
		auto& pipe = *pipePtr;
		pipe.dev = &dev;
		pipe.type = VK_PIPELINE_BIND_POINT_GRAPHICS;
		pipe.handle = pPipelines[i];
		pipe.layout = getPtr(dev, pCreateInfos[i].layout);
		pipe.renderPass = std::move(pres[i].rp);
		pipe.subpass = pci.subpass;

		// TODO: handle pNext infos.
		// - dynamic rendering

		pci.layout = pipe.layout->handle;

		[[maybe_unused]] auto colorAttachmentCount = 0u;
		auto hasDepthStencil = false;
		if(pipe.renderPass) {
			auto& subpassInfo = pipe.renderPass->desc.subpasses[pipe.subpass];
			colorAttachmentCount = subpassInfo.colorAttachmentCount;
			hasDepthStencil = !!subpassInfo.pDepthStencilAttachment;
		}

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

		if(pci.pDynamicState) {
			pipe.dynamicState = {
				pci.pDynamicState->pDynamicStates,
				pci.pDynamicState->pDynamicStates + pci.pDynamicState->dynamicStateCount
			};
		}

		if(!pipe.hasMeshShader) {
			// vertex input state ignored if dynamic vertex input is set
			if(!pipe.dynamicState.count(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) {
				dlg_assert(pci.pVertexInputState);
				pipe.vertexAttribs = {
					pci.pVertexInputState->pVertexAttributeDescriptions,
					pci.pVertexInputState->pVertexAttributeDescriptions + pci.pVertexInputState->vertexAttributeDescriptionCount
				};
				pipe.vertexBindings = {
					pci.pVertexInputState->pVertexBindingDescriptions,
					pci.pVertexInputState->pVertexBindingDescriptions + pci.pVertexInputState->vertexBindingDescriptionCount
				};

				pipe.vertexInputState = *pci.pVertexInputState;
			}

			pipe.inputAssemblyState = *pci.pInputAssemblyState;
		}

		if(!pci.pRasterizationState->rasterizerDiscardEnable) {
			pipe.multisampleState = *pci.pMultisampleState;
			pipe.viewportState = *pci.pViewportState;

			if(!pipe.dynamicState.count(VK_DYNAMIC_STATE_SCISSOR)) {
				pipe.scissors = {
					pipe.viewportState.pScissors,
					pipe.viewportState.pScissors + pipe.viewportState.scissorCount
				};
			}

			if(!pipe.dynamicState.count(VK_DYNAMIC_STATE_VIEWPORT)) {
				pipe.viewports = {
					pipe.viewportState.pViewports,
					pipe.viewportState.pViewports + pipe.viewportState.viewportCount
				};
			}

			pipe.hasDepthStencil = hasDepthStencil;
			if(pipe.hasDepthStencil) {
				pipe.depthStencilState = *pci.pDepthStencilState;
			}
		}

		if(pipe.hasTessellation) {
			dlg_assert(pci.pTessellationState);
			pipe.tessellationState = *pci.pTessellationState;
		}

		// NOTE: even if there are color attachments, pColorBlendState might
		// be null e.g. if rasterizerDiscardEnable is true.
		// We can't rely on comparison of pColorBlendState to null here,
		// might be set to invalid pointer.
		auto needsColorBlend =
			colorAttachmentCount != 0u &&
			!pci.pRasterizationState->rasterizerDiscardEnable;
		if(needsColorBlend) {
			dlg_assert(pci.pColorBlendState);
			pipe.blendAttachments = {
				pci.pColorBlendState->pAttachments,
				pci.pColorBlendState->pAttachments + pci.pColorBlendState->attachmentCount
			};

			pipe.colorBlendState = *pci.pColorBlendState;
		}

		fixPointers(pipe);

		pPipelines[i] = castDispatch<VkPipeline>(static_cast<Pipeline&>(pipe));

		auto newPipePtr = IntrusiveDerivedPtr<Pipeline>(pipePtr.get());
		dev.pipes.mustEmplace(pPipelines[i], std::move(newPipePtr));
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

	ThreadMemScope memScope;
	auto ncis = memScope.copy(pCreateInfos, createInfoCount);
	for(auto& nci : ncis) {
		nci.layout = get(device, nci.layout).handle;
		if(nci.basePipelineHandle) {
			auto& basePipe = get(dev, nci.basePipelineHandle);
			nci.basePipelineHandle = basePipe.handle;
		}
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

		auto pipePtr = IntrusivePtr<ComputePipeline>(new ComputePipeline());
		auto& pipe = *pipePtr;
		pipe.type = VK_PIPELINE_BIND_POINT_COMPUTE;
		pipe.dev = &dev;
		pipe.handle = pPipelines[i];
		pipe.layout = getPtr(dev, pCreateInfos[i].layout);
		pipe.stage = PipelineShaderStage(dev, pCreateInfos[i].stage);
		dlg_assert(pipe.stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);

		pPipelines[i] = castDispatch<VkPipeline>(static_cast<Pipeline&>(pipe));

		auto newPipePtr = IntrusiveDerivedPtr<Pipeline>(pipePtr.get());
		dev.pipes.mustEmplace(pPipelines[i], std::move(newPipePtr));
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

	auto pipe = mustMoveUnset(device, pipeline);
	auto& dev = *pipe->dev;

	// Pipeline destructor isn't virtual so we release the IntrusivePtr<Pipeline>
	// and construct an IntrusivePtr temporary with the correct type.
	// That will decrease the refCount (and potentially delete the object)
	// since its immediately destroyed again.
	switch(pipe->type) {
		case VK_PIPELINE_BIND_POINT_GRAPHICS:
			IntrusivePtr<GraphicsPipeline>(acquireOwnership,
				static_cast<GraphicsPipeline*>(pipe.release()));
			break;
		case VK_PIPELINE_BIND_POINT_COMPUTE:
			IntrusivePtr<ComputePipeline>(acquireOwnership,
				static_cast<ComputePipeline*>(pipe.release()));
			break;
		case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
			IntrusivePtr<RayTracingPipeline>(acquireOwnership,
				static_cast<RayTracingPipeline*>(pipe.release()));
			break;
		default:
			dlg_error("unreachable");
			break;
	}

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

	ThreadMemScope memScope;
	auto dslHandles = memScope.alloc<VkDescriptorSetLayout>(pCreateInfo->setLayoutCount);
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

span<const PipelineShaderStage> stages(const Pipeline& pipe) {
	switch(pipe.type) {
		case VK_PIPELINE_BIND_POINT_GRAPHICS:
			return static_cast<const GraphicsPipeline&>(pipe).stages;
		case VK_PIPELINE_BIND_POINT_COMPUTE:
			return {&static_cast<const ComputePipeline&>(pipe).stage, 1u};
		case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
			return static_cast<const RayTracingPipeline&>(pipe).stages;
		default:
			dlg_error("Invalid pipeline type: {}", pipe.type);
			return {};
	}
}

u32 getSpvExecutionModel(VkShaderStageFlagBits stage) {
	switch(stage) {
		case VK_SHADER_STAGE_VERTEX_BIT:
			return spv::ExecutionModelVertex;
		case VK_SHADER_STAGE_GEOMETRY_BIT:
			return spv::ExecutionModelGeometry;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
			return spv::ExecutionModelTessellationControl;
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			return spv::ExecutionModelTessellationEvaluation;
		case VK_SHADER_STAGE_FRAGMENT_BIT:
			return spv::ExecutionModelFragment;
		case VK_SHADER_STAGE_COMPUTE_BIT:
			return spv::ExecutionModelGLCompute;
		case VK_SHADER_STAGE_TASK_BIT_NV:
			return spv::ExecutionModelTaskNV;
		case VK_SHADER_STAGE_MESH_BIT_NV:
			return spv::ExecutionModelMeshNV;
		case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
			return spv::ExecutionModelRayGenerationKHR;
		case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
			return spv::ExecutionModelClosestHitKHR;
		case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
			return spv::ExecutionModelAnyHitKHR;
		case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
			return spv::ExecutionModelCallableKHR;
		case VK_SHADER_STAGE_MISS_BIT_KHR:
			return spv::ExecutionModelMissKHR;
		case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
			return spv::ExecutionModelIntersectionKHR;
		default:
			dlg_error("Unsupported stage {}", stage);
			return spv::ExecutionModelFragment;
	}
}

spc::Compiler& specializeSpirv(const PipelineShaderStage& stage) {
	auto execModel = getSpvExecutionModel(stage.stage);
	dlg_assert(stage.spirv);
	return specializeSpirv(*stage.spirv, stage.specialization,
		stage.entryPoint, execModel);
}

std::unique_ptr<spc::Compiler> copySpecializeSpirv(const PipelineShaderStage& stage) {
	auto execModel = getSpvExecutionModel(stage.stage);
	dlg_assert(stage.spirv);
	return copySpecializeSpirv(*stage.spirv, stage.specialization,
		stage.entryPoint, execModel);
}

GraphicsPipeline::~GraphicsPipeline() = default;

void fixPointers(GraphicsPipeline& pipe) {
	pipe.vertexInputState.pVertexAttributeDescriptions = pipe.vertexAttribs.data();
	pipe.vertexInputState.pVertexBindingDescriptions = pipe.vertexBindings.data();
	pipe.viewportState.pScissors = pipe.scissors.data();
	pipe.viewportState.pViewports = pipe.viewports.data();
	pipe.colorBlendState.pAttachments = pipe.blendAttachments.data();
}

// VK_KHR_ray_tracing_pipeline
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
		VkDevice                                    device,
		VkDeferredOperationKHR                      deferredOperation,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	ZoneScoped;
	auto& dev = getDevice(device);

	ThreadMemScope memScope;
	auto ncis = memScope.copy(pCreateInfos, createInfoCount);
	for(auto& nci : ncis) {
		nci.layout = get(device, nci.layout).handle;
		if(nci.basePipelineHandle) {
			auto& basePipe = get(dev, nci.basePipelineHandle);
			nci.basePipelineHandle = basePipe.handle;
		}

		if(nci.pLibraryInfo) {
			auto& copy = *memScope.allocRaw<VkPipelineLibraryCreateInfoKHR>();
			copy = *nci.pLibraryInfo;

			auto libHandles = memScope.alloc<VkPipeline>(copy.libraryCount);
			for(auto i = 0u; i < copy.libraryCount; ++i) {
				auto& lib = get(dev, copy.pLibraries[i]);
				libHandles[i] = lib.handle;
			}

			copy.pLibraries = libHandles.data();
			nci.pLibraryInfo = &copy;
		}
	}

	{
		ZoneScopedN("dispatch");
		auto res = dev.dispatch.CreateRayTracingPipelinesKHR(dev.handle,
			deferredOperation, pipelineCache, createInfoCount, ncis.data(),
			pAllocator, pPipelines);
		if(res != VK_SUCCESS &&
				res != VK_OPERATION_NOT_DEFERRED_KHR &&
				res != VK_OPERATION_DEFERRED_KHR &&
				res != VK_PIPELINE_COMPILE_REQUIRED_EXT) {
			dlg_trace("CreateRayTracingPipelinesKHR returned {} ({})", vk::name(res), res);
			return res;
		}
	}

	for(auto i = 0u; i < createInfoCount; ++i) {
		dlg_assert(pPipelines[i]);
		auto& ci = pCreateInfos[i];

		auto pipePtr = IntrusivePtr<RayTracingPipeline>(new RayTracingPipeline());
		auto& pipe = *pipePtr;
		pipe.type = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
		pipe.dev = &dev;
		pipe.handle = pPipelines[i];
		pipe.layout = getPtr(dev, ci.layout);

		if(ci.pDynamicState) {
			pipe.dynamicState = {
				ci.pDynamicState->pDynamicStates,
				ci.pDynamicState->pDynamicStates + ci.pDynamicState->dynamicStateCount
			};
		}

		for(auto i = 0u; i < ci.stageCount; ++i) {
			pipe.stages.emplace_back(dev, ci.pStages[i]);
		}

		for(auto i = 0u; i < ci.groupCount; ++i) {
			auto& src = ci.pGroups[i];
			auto& dst = pipe.groups.emplace_back();

			dst.anyHit = src.anyHitShader;
			dst.closestHit = src.closestHitShader;
			dst.general = src.generalShader;
			dst.intersection = src.intersectionShader;
			dst.type = src.type;
		}

		pPipelines[i] = castDispatch<VkPipeline>(static_cast<Pipeline&>(pipe));

		auto newPipePtr = IntrusiveDerivedPtr<Pipeline>(pipePtr.get());
		dev.pipes.mustEmplace(pPipelines[i], std::move(newPipePtr));
	}

	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingCaptureReplayShaderGroupHandlesKHR(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		uint32_t                                    firstGroup,
		uint32_t                                    groupCount,
		size_t                                      dataSize,
		void*                                       pData) {
	auto& pipe = get(device, pipeline);
	return pipe.dev->dispatch.GetRayTracingCaptureReplayShaderGroupHandlesKHR(
		pipe.dev->handle, pipe.handle, firstGroup, groupCount, dataSize, pData);
}

VKAPI_ATTR VkDeviceSize VKAPI_CALL GetRayTracingShaderGroupStackSizeKHR(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		uint32_t                                    group,
		VkShaderGroupShaderKHR                      groupShader) {
	auto& pipe = get(device, pipeline);
	return pipe.dev->dispatch.GetRayTracingShaderGroupStackSizeKHR(
		pipe.dev->handle, pipe.handle, group, groupShader);
}

VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingShaderGroupHandlesKHR(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		uint32_t                                    firstGroup,
		uint32_t                                    groupCount,
		size_t                                      dataSize,
		void*                                       pData) {
	auto& pipe = get(device, pipeline);
	return pipe.dev->dispatch.GetRayTracingShaderGroupHandlesKHR(
		pipe.dev->handle, pipe.handle, firstGroup, groupCount, dataSize, pData);
}

} // namespace vil
