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

// util
std::string extractString(span<const u32> spirv) {
	std::string ret;
	for(auto w : spirv) {
		for (auto j = 0u; j < 4; j++, w >>= 8) {
			char c = w & 0xff;
			if(c == '\0') {
				return ret;
			}

			ret += c;
		}
	}

	dlg_error("Unterminated SPIR-V string");
	return {};
}

bool isOpInSection8(spv::Op op) {
	switch(op) {
		case spv::Op::OpDecorate:
		case spv::Op::OpMemberDecorate:
		case spv::Op::OpDecorationGroup:
		case spv::Op::OpGroupDecorate:
		case spv::Op::OpGroupMemberDecorate:
		case spv::Op::OpDecorateId:
		case spv::Op::OpDecorateString:
		case spv::Op::OpMemberDecorateString:
			return true;
		default:
			return false;
	}
}

VkShaderModule patchVertexShaderXfb(Device& dev, const std::vector<u32>& spirv,
		const char* entryPoint) {
	ZoneScoped;

	// parse spirv
	if(spirv.size() < 5) {
		dlg_error("spirv to small");
		return {};
	}

	if(spirv[0] != 0x07230203) {
		dlg_error("Invalid spirv magic number. Endianess troubles?");
		return {};
	}

	// auto version = spirv[1];
	// auto generator = spirv[2];
	// auto bound = spirv[3];

	std::vector<u32> newDecos;
	std::vector<u32> patched {spirv.begin(), spirv.begin() + 5};
	patched.reserve(spirv.size());

	auto addedCap = false;
	auto addedExecutionMode = false;

	auto section = 0u;
	auto entryPointID = u32(-1);
	auto insertDecosPos = u32(-1);

	auto offset = 5u;
	while(offset < spirv.size()) {
		auto first = spirv[offset];
		auto op = spv::Op(first & 0xFFFFu);
		auto wordCount = first >> 16u;

		// We need to add the Xfb Execution mode to our entry point.
		if(section == 5u && op != spv::Op::OpEntryPoint) {
			dlg_assert_or(entryPointID != u32(-1), return {});

			section = 6u;
			patched.push_back((3u << 16) | u32(spv::Op::OpExecutionMode));
			patched.push_back(entryPointID);
			patched.push_back(u32(spv::ExecutionMode::Xfb));

			addedExecutionMode = true;
		}

		// check if we have reached section 8
		if(isOpInSection8(op) && insertDecosPos == u32(-1)) {
			dlg_assert(section < 8);
			section = 8u;
			insertDecosPos = u32(patched.size());
		}

		for(auto i = 0u; i < wordCount; ++i) {
			patched.push_back(spirv[offset + i]);
		}

		// We need to add the TransformFeedback capability
		if(op == spv::Op::OpCapability) {
			dlg_assert(section <= 1u);
			section = 1u;

			dlg_assert(wordCount == 2);
			auto cap = spv::Capability(spirv[offset + 1]);

			// The shader *must* declare shader capability exactly once.
			// We add the transformFeedback cap just immediately after that.
			if(cap == spv::Capability::Shader) {
				dlg_assert(!addedCap);
				patched.push_back((2u << 16) | u32(spv::Op::OpCapability));
				patched.push_back(u32(spv::Capability::TransformFeedback));
				addedCap = true;
			}

			// When the shader itself declared that capability, there is
			// nothing we can do.
			// TODO: maybe in some cases shaders just declare that cap but
			// don't use it? In that case we could still patch in our own values
			if(cap == spv::Capability::TransformFeedback) {
				dlg_debug("Shader is already using transform feedback!");
				return {};
			}
		}

		// We need to find the id of the entry point
		if(op == spv::Op::OpEntryPoint) {
			dlg_assert(section <= 5u);
			section = 5u;

			dlg_assert(wordCount >= 4);
			auto length = wordCount - 3;
			auto name = extractString(span(spirv).subspan(offset + 3, length));
			if(!name.empty() && name == entryPoint) {
				entryPointID = spirv[offset + 2];
			}
		}

		// We need to add our xfb decorations to outputs from the shader stage
		// TODO: we just capture the Position BuiltIn for now
		/*
		if(op == spv::Op::OpVariable) {
			dlg_assert_or(wordCount >= 4, return {});
			// auto resType = spirv[offset + 1];
			auto resID = spirv[offset + 2];
			auto storage = spv::StorageClass(spirv[offset + 3]);
			if(storage == spv::StorageClass::Output) {
				// TODO: values. Also store them somewhere
				newDecos.push_back(4u << 16 | (u32(spv::Op::OpDecorate) << 16));
				newDecos.push_back(resID);
				newDecos.push_back(u32(spv::Decoration::XfbBuffer));
				newDecos.push_back(0u);

				newDecos.push_back(4u << 16 | u32(spv::Op::OpDecorate));
				newDecos.push_back(resID);
				newDecos.push_back(u32(spv::Decoration::XfbStride));
				newDecos.push_back(4u);

				newDecos.push_back(4u << 16 | u32(spv::Op::OpDecorate));
				newDecos.push_back(resID);
				newDecos.push_back(u32(spv::Decoration::Offset));
				newDecos.push_back(0u);
			}
		}
		*/

		if(op == spv::Op::OpDecorate) {
			dlg_assert(section <= 8u);
			section = 8u;

			dlg_assert(wordCount >= 3);
			auto target = spirv[offset + 1];
			auto decoration = spv::Decoration(spirv[offset + 2]);
			if(decoration == spv::Decoration::BuiltIn) {
				dlg_assert(wordCount == 4);
				auto builtin = spv::BuiltIn(spirv[offset + 3]);
				if(builtin == spv::BuiltIn::Position) {
					auto addDeco = [&](spv::Decoration deco, u32 value) {
						newDecos.push_back((4u << 16) | u32(spv::Op::OpDecorate));
						newDecos.push_back(target);
						newDecos.push_back(u32(deco));
						newDecos.push_back(value);
					};

					addDeco(spv::Decoration::XfbBuffer, 0u);
					addDeco(spv::Decoration::XfbStride, 16u); // vec4
					addDeco(spv::Decoration::Offset, 0u);
				}
			}
		} else if(op == spv::Op::OpMemberDecorate) {
			dlg_assert(section <= 8u);
			section = 8u;

			dlg_assert(wordCount >= 4);
			auto structType = spirv[offset + 1];
			auto member = spirv[offset + 2];
			auto decoration = spv::Decoration(spirv[offset + 3]);
			if(decoration == spv::Decoration::BuiltIn) {
				dlg_assert(wordCount == 5);
				auto builtin = spv::BuiltIn(spirv[offset + 4]);
				if(builtin == spv::BuiltIn::Position) {
					auto addMemberDeco = [&](spv::Decoration deco, u32 value) {
						newDecos.push_back((5u << 16) | u32(spv::Op::OpMemberDecorate));
						newDecos.push_back(structType);
						newDecos.push_back(member);
						newDecos.push_back(u32(deco));
						newDecos.push_back(value);
					};

					addMemberDeco(spv::Decoration::XfbBuffer, 0u);
					addMemberDeco(spv::Decoration::XfbStride, 16u); // vec4
					addMemberDeco(spv::Decoration::Offset, 0u);
				}
			}
		}

		offset += wordCount;
	}

	if(!addedCap || !addedExecutionMode || newDecos.empty() || insertDecosPos == u32(-1)) {
		dlg_warn("Could not inject xfb into shader. Likely error inside vil. "
			"capability: {}, executionMode: {}, newDecos.size(): {}, decosPos: {}",
			addedCap, addedExecutionMode, newDecos.size(), insertDecosPos);
		return {};
	}

	patched.insert(patched.begin() + insertDecosPos, newDecos.begin(), newDecos.end());

	VkShaderModuleCreateInfo ci {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.pCode = patched.data();
	ci.codeSize = patched.size() * 4;

	VkShaderModule mod;
	auto res = dev.dispatch.CreateShaderModule(dev.handle, &ci, nullptr, &mod);
	if(res != VK_SUCCESS) {
		dlg_error("xfb CreateShaderModule: {} (res)", vk::name(res), res);
		return {};
	}

	return mod;
}

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
		bool useXfb {};
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

		// transform feedback isn't supported for multiview graphics pipelines
		auto& rp = dev.renderPasses.get(nci.renderPass);
		pre.stages = {nci.pStages, nci.stageCount};
		pre.useXfb = dev.transformFeedback &&
			!hasChain(*rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
		u32 xfbVertexStageID = u32(-1);

		auto& stages = stagesVecs.emplace_back();
		for(auto s = 0u; s < nci.stageCount; ++s) {
			auto src = nci.pStages[s];
			if(src.stage == VK_SHADER_STAGE_VERTEX_BIT && pre.useXfb) {
				dlg_assert(xfbVertexStageID == u32(-1));
				xfbVertexStageID = u32(stages.size());
			}

			if(src.stage == VK_SHADER_STAGE_GEOMETRY_BIT ||
					src.stage == VK_SHADER_STAGE_MESH_BIT_NV ||
					src.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
					src.stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
				// Builtin Position might not be present
				dlg_trace("xfb currently not supported for mesh/geometry/tessellation pipes");
				pre.useXfb = false;
			}

			stages.push_back(src);
		}

		pre.useXfb &= xfbVertexStageID != u32(-1);
		if (pre.useXfb) {
			auto& stage = stages[xfbVertexStageID];
			auto& mod = dev.shaderModules.get(stage.module);

			// PERF: we lock the device mutex here for too long,
			// shader patching and module creation can take a long time
			// and pipeline creation is often parallelized. Maybe create a
			// per-shader-module mutex just for this place? Or handle
			// the case where we patch the vertex shader and then find
			// it's already been set by another thread in the meantime.
			std::lock_guard lock(dev.mutex);
			if(!mod.xfbVertShader) {
				mod.xfbVertShader = patchVertexShaderXfb(dev,
					mod.code->spv, stage.pName);
			}

			if(mod.xfbVertShader) {
				stage.module = mod.xfbVertShader;
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
		pipe.xfbPatched = pres[i].useXfb;

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

} // namespace vil
