#include <command/match.hpp>
#include <command/commands.hpp>
#include <threadContext.hpp>
#include <ds.hpp>
#include <rp.hpp>
#include <accelStruct.hpp>
#include <swapchain.hpp>
#include <lmm.hpp>
#include <sync.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <util/dlg.hpp>
#include <util/profiling.hpp>

// We interpret matching of two command sequences submitted
// to the gpu as an instance of the common longest subsequence
// problem (LCS). We solve it via the custom lazy-matrix-march
// algorithm (LMM), see lmm.hpp.
// In our case, we want to match hierarchies and therefore
// have recursive instances of LMM.

// TODO:
// - a lot of commands are still missing valid match() implementations.
//   some commands (bind, sync) will need contextual information, i.e.
//   an external match implementation. Or maybe just having 'prev'
//   links in addition to 'next' is already enough? probably not,
//   the commands itself also should not do the iteration, should not
//   know about other commands.

namespace vil {

unsigned popcount(u64 val) {
#ifdef __GNUC__
	return __builtin_popcount(val);
#else
	auto ret = 0u;
	while(val != 0u) {
		if(val & 1) {
			++ret;
		}

		val = val >> 1;
	}
	return ret;
#endif // __GNUC__
}

template<typename T>
void addBits(MatchVal& m, T a, T b) {
	auto ua = u64(a);
	auto ub = u64(b);

	auto maxBits = std::max(popcount(ua), popcount(ub));
	if(maxBits == 0u) {
		m.total += 0;
		m.match += 0;
	} else {
		m.total += maxBits;
		m.match += popcount(ua ^ ub) / float(maxBits);
	}
}

bool equal(const RenderPassDesc& a, const RenderPassDesc& b) {
	if(a.subpasses.size() != b.subpasses.size() ||
			a.attachments.size() != b.attachments.size()) {
		return false;
	}

	// compare attachments
	for(auto i = 0u; i < a.attachments.size(); ++i) {
		auto& attA = a.attachments[i];
		auto& attB = b.attachments[i];

		if(attA.format != attB.format ||
				attA.loadOp != attB.loadOp ||
				attA.storeOp != attB.storeOp ||
				attA.initialLayout != attB.initialLayout ||
				attA.finalLayout != attB.finalLayout ||
				attA.stencilLoadOp != attB.stencilLoadOp ||
				attA.stencilStoreOp != attB.stencilStoreOp ||
				attA.samples != attB.samples) {
			return false;
		}
	}

	// compare subpasses
	auto attRefsSame = [](const VkAttachmentReference2& a, const VkAttachmentReference2& b) {
		return a.attachment == b.attachment && (a.attachment == VK_ATTACHMENT_UNUSED ||
				a.aspectMask == b.aspectMask);
	};

	for(auto i = 0u; i < a.subpasses.size(); ++i) {
		auto& subA = a.subpasses[i];
		auto& subB = b.subpasses[i];

		if(subA.colorAttachmentCount != subB.colorAttachmentCount ||
				subA.preserveAttachmentCount != subB.preserveAttachmentCount ||
				bool(subA.pDepthStencilAttachment) != bool(subB.pDepthStencilAttachment) ||
				bool(subA.pResolveAttachments) != bool(subB.pResolveAttachments) ||
				subA.inputAttachmentCount != subB.inputAttachmentCount ||
				subA.pipelineBindPoint != subB.pipelineBindPoint) {
			return false;
		}

		for(auto j = 0u; j < subA.colorAttachmentCount; ++j) {
			if(!attRefsSame(subA.pColorAttachments[j], subB.pColorAttachments[j])) {
				return false;
			}
		}

		for(auto j = 0u; j < subA.inputAttachmentCount; ++j) {
			if(!attRefsSame(subA.pInputAttachments[j], subB.pInputAttachments[j])) {
				return false;
			}
		}

		for(auto j = 0u; j < subA.preserveAttachmentCount; ++j) {
			if(subA.pPreserveAttachments[j] != subB.pPreserveAttachments[j]) {
				return false;
			}
		}

		if(subA.pResolveAttachments) {
			for(auto j = 0u; j < subA.colorAttachmentCount; ++j) {
				if(!attRefsSame(subA.pResolveAttachments[j], subB.pResolveAttachments[j])) {
					return false;
				}
			}
		}

		if(subA.pDepthStencilAttachment &&
				!attRefsSame(*subA.pDepthStencilAttachment, *subB.pDepthStencilAttachment)) {
			return false;
		}
	}

	// TODO: compare dependencies?
	return true;
}

bool conflicting(const PipelineLayout& a, const PipelineLayout& b) {
	// TODO: relax pcr-same requirement?
	//   at least make order insensitive?
	if(a.pushConstants.size() != b.pushConstants.size()) {
		return true;
	}

	for(auto i = 0u; i < a.pushConstants.size(); ++i) {
		auto& pca = a.pushConstants[i];
		auto& pcb = b.pushConstants[i];
		if(pca.offset != pcb.offset ||
				pca.size != pcb.size ||
				pca.stageFlags != pcb.stageFlags) {
			return true;
		}
	}

	// NOTE: we are intentionally lax about descriptor bindings
	// compatibility to allow changes in pipeline reloads.
	// We only disallow *conflicting* bindings
	auto count = std::min(a.descriptors.size(),
		b.descriptors.size());
	for(auto i = 0u; i < count; ++i) {
		auto* dsa = a.descriptors[i].get();
		auto* dsb = b.descriptors[i].get();

		if(!dsa || !dsb) {
			continue;
		}

		if(conflicting(*dsa, *dsb)) {
			return true;
		}
	}

	return false;
}

bool equal(const ShaderModule& a, const ShaderModule& b) {
	// Treating shader modules with the same name as equal even when they have
	// different content is useful e.g. for shader reloading.
	return a.spirvHash == b.spirvHash ||
		(!a.name.empty() && a.name == b.name);
}

MatchVal matchStages(span<const PipelineShaderStage> a,
		span<const PipelineShaderStage> b, float weight = 1.0) {

	MatchVal ret;
	ret.total += std::max(a.size(), b.size()) * weight;

	for(auto& stageA : a) {
		for(auto& stageB : b) {
			dlg_assert(stageA.spirv);
			dlg_assert(stageB.spirv);

			// TODO: also compare specialization somehow?
			if(stageA.stage == stageB.stage &&
					equal(*stageA.spirv, *stageB.spirv) &&
					stageA.entryPoint == stageB.entryPoint) {
				ret.match += weight;
				break;
			}
		}
	}

	return ret;
}

MatchVal matchPipe(const GraphicsPipeline& a, const GraphicsPipeline& b) {
	if(!equal(a.renderPass->desc, b.renderPass->desc) || a.subpass != b.subpass) {
		return MatchVal::noMatch();
	}

	auto ret = matchStages(a.stages, b.stages, 10.f);

	// vertex attribs
	ret.total += std::max(a.vertexAttribs.size(), b.vertexAttribs.size());
	for(auto& attribA : a.vertexAttribs) {
		for(auto& attribB : b.vertexAttribs) {
			if(attribA.binding == attribB.binding &&
					attribA.location == attribB.location &&
					attribA.format == attribB.format &&
					attribA.offset == attribB.offset) {
				ret.match += 1.f;
				break;
			}
		}
	}

	// vertex bindings
	ret.total += std::max(a.vertexBindings.size(), b.vertexBindings.size());
	for(auto& bindingA : a.vertexBindings) {
		for(auto& bindingB : b.vertexBindings) {
			if(bindingA.binding == bindingB.binding &&
					bindingA.inputRate == bindingB.inputRate &&
					bindingA.stride == bindingB.stride) {
				ret.match += 1.f;
				break;
			}
		}
	}

	// dynamic states
	ret.total += std::max(a.dynamicState.size(), b.dynamicState.size());
	for(auto& dynState : a.dynamicState) {
		if(b.dynamicState.count(dynState)) {
			ret.match += 1.f;
		}
	}

	// depth stencil
	// TODO: don't compare the values that are present in dynamic state?
	add(ret, a.hasDepthStencil, b.hasDepthStencil);
	if(a.hasDepthStencil && b.hasDepthStencil) {
		add(ret, a.depthStencilState.depthTestEnable, b.depthStencilState.depthTestEnable);
		if(a.depthStencilState.depthTestEnable && b.depthStencilState.depthTestEnable) {
			add(ret, a.depthStencilState.depthCompareOp, b.depthStencilState.depthCompareOp);
		}

		add(ret, a.depthStencilState.depthBoundsTestEnable, b.depthStencilState.depthBoundsTestEnable);
		if(a.depthStencilState.depthBoundsTestEnable && b.depthStencilState.depthBoundsTestEnable) {
			add(ret, a.depthStencilState.minDepthBounds, b.depthStencilState.minDepthBounds);
			add(ret, a.depthStencilState.maxDepthBounds, b.depthStencilState.maxDepthBounds);
		}

		add(ret, a.depthStencilState.stencilTestEnable, b.depthStencilState.stencilTestEnable);
		if(a.depthStencilState.stencilTestEnable && b.depthStencilState.stencilTestEnable) {
			add(ret, a.depthStencilState.front.failOp, b.depthStencilState.front.failOp);
			add(ret, a.depthStencilState.front.passOp, b.depthStencilState.front.passOp);
			add(ret, a.depthStencilState.front.compareMask, b.depthStencilState.front.compareMask);
			add(ret, a.depthStencilState.front.writeMask, b.depthStencilState.front.writeMask);
			add(ret, a.depthStencilState.front.compareOp, b.depthStencilState.front.compareOp);
			add(ret, a.depthStencilState.front.reference, b.depthStencilState.front.reference);
			add(ret, a.depthStencilState.front.depthFailOp, b.depthStencilState.front.depthFailOp);
		}

		add(ret, a.depthStencilState.depthWriteEnable, b.depthStencilState.depthWriteEnable);
		addBits(ret, a.depthStencilState.flags, b.depthStencilState.flags);
	}

	// color blend
	add(ret, a.colorBlendState.logicOpEnable, b.colorBlendState.logicOpEnable);

	auto blendCount = std::min(a.colorBlendState.attachmentCount, b.colorBlendState.attachmentCount);
	ret.total += std::max(a.colorBlendState.attachmentCount, b.colorBlendState.attachmentCount) - blendCount;

	for(auto i = 0u; i < blendCount; ++i) {
		auto& blendA = a.colorBlendState.pAttachments[i];
		auto& blendB = b.colorBlendState.pAttachments[i];
		add(ret, blendA.blendEnable, blendB.blendEnable);
		if(blendA.blendEnable && blendB.blendEnable) {
			add(ret, blendA.alphaBlendOp, blendB.alphaBlendOp);
			add(ret, blendA.colorBlendOp, blendB.colorBlendOp);
			add(ret, blendA.dstAlphaBlendFactor, blendB.dstAlphaBlendFactor);
			add(ret, blendA.srcAlphaBlendFactor, blendB.srcAlphaBlendFactor);
			add(ret, blendA.dstColorBlendFactor, blendB.dstColorBlendFactor);
			add(ret, blendA.srcColorBlendFactor, blendB.srcColorBlendFactor);
			add(ret, blendA.colorWriteMask, blendB.colorWriteMask);
		}
	}

	add(ret, a.inputAssemblyState.primitiveRestartEnable, b.inputAssemblyState.primitiveRestartEnable);
	add(ret, a.inputAssemblyState.topology, b.inputAssemblyState.topology);

	add(ret, a.rasterizationState.cullMode, b.rasterizationState.cullMode);
	add(ret, a.rasterizationState.rasterizerDiscardEnable, b.rasterizationState.rasterizerDiscardEnable);
	add(ret, a.rasterizationState.polygonMode, b.rasterizationState.polygonMode);
	add(ret, a.rasterizationState.frontFace, b.rasterizationState.frontFace);
	add(ret, a.rasterizationState.depthBiasEnable, b.rasterizationState.depthBiasEnable);
	if(a.rasterizationState.depthBiasEnable && b.rasterizationState.depthBiasEnable) {
		add(ret, a.rasterizationState.depthBiasClamp, b.rasterizationState.depthBiasClamp);
		add(ret, a.rasterizationState.depthBiasSlopeFactor, b.rasterizationState.depthBiasSlopeFactor);
		add(ret, a.rasterizationState.depthBiasConstantFactor, b.rasterizationState.depthBiasConstantFactor);
	}

	if(a.rasterizationState.polygonMode == VK_POLYGON_MODE_LINE &&
			b.rasterizationState.polygonMode == VK_POLYGON_MODE_LINE) {
		add(ret, a.rasterizationState.lineWidth, b.rasterizationState.lineWidth);
	}

	add(ret, a.multisampleState.rasterizationSamples, b.multisampleState.rasterizationSamples);

	// TODO: other state. scissor? viewports?

	return ret;
}

MatchVal matchPipe(const RayTracingPipeline& a, const RayTracingPipeline& b) {
	// TODO: match other state
	return matchStages(a.stages, b.stages, 10.f);
}

MatchVal matchPipe(const ComputePipeline& a, const ComputePipeline& b) {
	return matchStages({{a.stage}}, {{b.stage}}, 10.f);
}

MatchVal matchDeep(const Pipeline& a, const Pipeline& b) {
	if(a.type != b.type) {
		return MatchVal::noMatch();
	}

	// check for layout compat
	if(!a.layout || !b.layout || conflicting(*a.layout.get(), *b.layout.get())) {
		return MatchVal::noMatch();
	}

	switch(a.type) {
		case VK_PIPELINE_BIND_POINT_GRAPHICS:
			return matchPipe(
				static_cast<const GraphicsPipeline&>(a),
				static_cast<const GraphicsPipeline&>(b));
		case VK_PIPELINE_BIND_POINT_COMPUTE:
			return matchPipe(
				static_cast<const ComputePipeline&>(a),
				static_cast<const ComputePipeline&>(b));
		case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
			return matchPipe(
				static_cast<const RayTracingPipeline&>(a),
				static_cast<const RayTracingPipeline&>(b));
		default:
			dlg_error("unsupported pipeline type");
			return {1.f, 1.f};
	}
}

float eval(const MatchVal& m) {
	dlg_assertm(valid(m), "match {}, total {}", m.match, m.total);
	if(m.match == 0.f) { // no match
		// For match=total=0, we return 1.f by design.
		// This means we had no data to match on.
		// Imagine an empty span, we want to consider two of them similar.
		return m.total == 0.f ? 1.f : 0.f;
	}

	return m.match / m.total;
}

bool valid(const MatchVal& m) {
	return m.total == -1.f || m.match <= m.total;
}

bool addNonEmpty(MatchVal& m, std::string_view str1, std::string_view str2, float w) {
	if(str1.empty() && str2.empty()) {
		return true;
	}

	return add(m, str1, str2, w);
}

MatchVal match(MatchType, const VkMultiDrawInfoEXT& a, const VkMultiDrawInfoEXT& b) {
	MatchVal m;
	add(m, a.firstVertex, b.firstVertex);
	add(m, a.vertexCount, b.vertexCount);
	return m;
}

MatchVal match(MatchType, const VkMultiDrawIndexedInfoEXT& a, const VkMultiDrawIndexedInfoEXT& b) {
	MatchVal m;
	add(m, a.firstIndex, b.firstIndex);
	add(m, a.indexCount, b.indexCount);
	add(m, a.vertexOffset, b.vertexOffset);
	return m;
}

// TODO: code duplication with commands.cpp, but moving it to match.hpp
// instead of having it twice causes weird issues with overloading and name
// lookup
template<typename T>
void addSpanOrderedStrict(MatchVal& m, MatchType mt, span<T> a, span<T> b, float weight = 1.0) {
	m.total += weight;

	if(a.size() != b.size()) {
		return;
	}

	if(a.empty()) {
		m.match += weight;
		return;
	}

	MatchVal accum {};
	for(auto i = 0u; i < a.size(); ++i) {
		auto res = match(mt, a[i], b[i]);
		if(noMatch(res)) {
			accum.total += 1.0;
		} else {
			accum.match += res.match;
			accum.total += res.total;
		}
	}

	// TODO: maybe better to make weight also dependent on size?
	// could add flag/param for that behavior.
	m.match += weight * eval(accum);
}

MatchVal match(MatchType, const DescriptorSetLayout::Binding& a,
		const DescriptorSetLayout::Binding& b) {
	MatchVal m;
	if(!add(m, a.descriptorType, b.descriptorType)) {
		return m;
	}

	add(m, a.descriptorCount, b.descriptorCount);
	add(m, a.flags, b.flags);
	add(m, a.stageFlags, b.stageFlags);
	add(m, a.immutableSamplers ? true : false,
		b.immutableSamplers ? true : false);
	return m;
}

void add(MatchVal& m, const DescriptorSetLayout& a, const DescriptorSetLayout& b) {
	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.flags, b.flags);
	addSpanOrderedStrict(m, MatchType::deep,
		span<const DescriptorSetLayout::Binding>(a.bindings),
		span<const DescriptorSetLayout::Binding>(b.bindings),
		float(std::max(a.bindings.size(), b.bindings.size())));
}

void add(MatchVal& m, const VkImageSubresourceRange& a, const VkImageSubresourceRange& b,
		float weight) {
	add(m, a.aspectMask, b.aspectMask, weight);
	add(m, a.baseArrayLayer, b.baseArrayLayer, weight);
	add(m, a.baseMipLevel, b.baseMipLevel, weight);
	add(m, a.levelCount, b.levelCount, weight);
	add(m, a.layerCount, b.layerCount, weight);
}

MatchVal match(MatchType, const VkImageSubresourceRange& a, const VkImageSubresourceRange& b) {
	MatchVal ret;
	add(ret, a, b);
	return ret;
}

void add(MatchVal& m, const VkImageSubresourceLayers& a, const VkImageSubresourceLayers& b,
		float weight) {
	add(m, a.aspectMask, b.aspectMask, weight);
	add(m, a.baseArrayLayer, b.baseArrayLayer, weight);
	add(m, a.mipLevel, b.mipLevel, weight);
	add(m, a.layerCount, b.layerCount, weight);
}

MatchVal match(MatchType, const VkImageSubresourceLayers& a, const VkImageSubresourceLayers& b) {
	MatchVal ret;
	add(ret, a, b);
	return ret;
}

template<typename Handle, typename = decltype(matchDeep(std::declval<Handle>(), std::declval<Handle>()))>
MatchVal match(MatchType mt, const Handle* a, const Handle* b) {
	if(!a || !b) {
		return MatchVal::noMatch();
	}

	if(a == b) {
		return {1.f, 1.f};
	}

	if(mt == MatchType::identity) {
		return MatchVal::noMatch();
	}

	// When handles are named differently, we always assume them to be
	// different. When both are unnamed, we only do a deep-match in the
	// respective mode
	if(a->name != b->name || (mt == MatchType::mixed && a->name.empty())) {
		return MatchVal::noMatch();
	}

	auto ret = matchDeep(*a, *b);
	if(!noMatch(ret)) {
		ret.total += 1; // reduce match rating since identity wasn't same
	}

	dlg_assert(valid(ret));

	return ret;
}

// Bad idea, couldn't detect noMatch()
// template<typename T>
// void add(MatchVal& m, MatchType mt, const T& a, const T& b) {
// 	MatchVal tmp = match(mt, a, b);
// 	m.total += tmp.total;
// 	m.match += tmp.match;
// }

MatchVal matchDeep(const Event& a, const Event& b) {
	(void) a;
	(void) b;
	// TODO: any criteria?
	return {1.f, 1.f};
}

MatchVal matchDeep(const AccelStruct& a, const AccelStruct& b) {
	if(a.type != b.type) {
		return MatchVal::noMatch();
	}

	// TODO: any other criteria?

	return {1.f, 1.f};
}

MatchVal matchDeep(const Image& a, const Image& b) {
	MatchVal m;
	addNonEmpty(m, a.name, b.name, 10.f);

	if(a.swapchain || b.swapchain) {
		// NOTE: deep-matching swapchains does not really make sense
		add(m, a.swapchain, b.swapchain, 3.f);
	}

	add(m, a.ci.extent.width, b.ci.extent.width);
	add(m, a.ci.extent.height, b.ci.extent.height);
	add(m, a.ci.extent.depth, b.ci.extent.depth);
	add(m, a.ci.arrayLayers, b.ci.arrayLayers);
	add(m, a.ci.mipLevels, b.ci.mipLevels);
	add(m, a.ci.imageType, b.ci.imageType);
	add(m, a.ci.tiling, b.ci.tiling);
	add(m, a.ci.sharingMode, b.ci.sharingMode);
	add(m, a.ci.samples, b.ci.samples);
	add(m, a.ci.format, b.ci.format);
	addBits(m, a.ci.flags, b.ci.flags);
	addBits(m, a.ci.usage, b.ci.usage);
	return m;
}

MatchVal matchDeep(const ImageView& a, const ImageView& b) {
	auto m = match(MatchType::deep, a.img, b.img);
	if(m.match == 0.f || m.total == -1.f) {
		return MatchVal::noMatch();
	}

	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.ci.components.a, b.ci.components.a, 0.25f);
	add(m, a.ci.components.r, b.ci.components.r, 0.25f);
	add(m, a.ci.components.g, b.ci.components.g, 0.25f);
	add(m, a.ci.components.b, b.ci.components.b, 0.25f);
	add(m, a.ci.format, b.ci.format);
	add(m, a.ci.viewType, b.ci.viewType);
	add(m, a.ci.subresourceRange, b.ci.subresourceRange);

	return m;
}

MatchVal matchDeep(const Buffer& a, const Buffer& b) {
	MatchVal m;

	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.ci.size, b.ci.size);
	add(m, a.ci.usage, b.ci.usage);
	add(m, a.ci.flags, b.ci.flags);
	add(m, a.ci.sharingMode, b.ci.sharingMode);

	return m;
}

MatchVal matchDeep(const BufferView& a, const BufferView& b) {
	auto m = match(MatchType::deep, a.buffer, b.buffer);
	if(m.match == 0.f || m.total == -1.f) {
		return MatchVal::noMatch();
	}

	addNonEmpty(m, a.name, b.name, 20.f);
	add(m, a.ci.format, b.ci.format);
	add(m, a.ci.offset, b.ci.offset);
	add(m, a.ci.range, b.ci.range);
	add(m, a.ci.flags, b.ci.flags);

	return m;
}

MatchVal matchDeep(const Sampler& a, const Sampler& b) {
	MatchVal m;
	add(m, a.ci.addressModeU, b.ci.addressModeU);
	add(m, a.ci.addressModeV, b.ci.addressModeV);
	add(m, a.ci.addressModeW, b.ci.addressModeW);
	add(m, a.ci.mipmapMode, b.ci.mipmapMode);
	add(m, a.ci.borderColor, b.ci.borderColor);
	add(m, a.ci.magFilter, b.ci.magFilter);
	add(m, a.ci.minFilter, b.ci.minFilter);
	add(m, a.ci.maxLod, b.ci.maxLod);
	add(m, a.ci.minLod, b.ci.minLod);
	add(m, a.ci.mipLodBias, b.ci.mipLodBias);
	add(m, a.ci.unnormalizedCoordinates, b.ci.unnormalizedCoordinates);

	add(m, a.ci.compareEnable, b.ci.compareEnable);
	if(a.ci.compareEnable && b.ci.compareEnable) {
		add(m, a.ci.compareOp, b.ci.compareOp);
	}

	add(m, a.ci.anisotropyEnable, b.ci.anisotropyEnable);
	if(a.ci.anisotropyEnable && b.ci.anisotropyEnable) {
		add(m, a.ci.maxAnisotropy, b.ci.maxAnisotropy);
	}

	return m;
}

MatchVal match(MatchType mt, const DescriptorStateRef& a, const DescriptorStateRef& b) {
	dlg_assert(a.layout);
	dlg_assert(b.layout);

	// we expect them to have the same layout since they must
	// be bound for commands with the same pipeline
	dlg_assert_or(!conflicting(*a.layout, *b.layout), return MatchVal::noMatch());

	// fast path: full match since same descriptorSet
	if(a.data == b.data) {
		auto count = float(totalDescriptorCount(a));
		return MatchVal{count, count};
	}

	// iterate over bindings
	MatchVal m;
	auto count = std::min(a.layout->bindings.size(), b.layout->bindings.size());
	for(auto bindingID = 0u; bindingID < count; ++bindingID) {
		// they can have different size, when variable descriptor count is used
		auto sizeA = descriptorCount(a, bindingID);
		auto sizeB = descriptorCount(b, bindingID);

		if(sizeA == 0u || sizeB == 0u) {
			continue;
		}

		// must have the same type
		auto dsType = a.layout->bindings[bindingID].descriptorType;
		dlg_assert_or(a.layout->bindings[bindingID].descriptorType ==
			b.layout->bindings[bindingID].descriptorType, continue);

		if(dsType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
			// This might seem like a low weight but the bytewise
			// comparison isn't the best anyways. Counting the number
			// of equal bytes or weighing this by the block size
			// would be bad.
			m.total += 1;
		} else {
			// sizeA might not be the same as sizeB for variable descriptor counts
			m.total += std::max(sizeA, sizeB);
		}

		// if samplers or image/buffers views are different we check them for
		// semantic equality as well. Applications sometimes create
		// them lazily/on-demand or stuff like that.

		auto dsCat = vil::category(dsType);
		if(dsCat == DescriptorCategory::image) {
			auto bindingsA = images(a, bindingID);
			auto bindingsB = images(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];

				MatchVal combined;

				if(needsSampler(dsType)) {
					combined.total += 1;
					if(bindA.sampler == bindB.sampler) {
						combined.match += 1;
					} else {
						auto samplerMatch = match(mt, bindA.sampler, bindB.sampler);
						if(noMatch(samplerMatch)) {
							add(combined, samplerMatch);
						} else {
							combined.total += 1.f;
						}
					}
				}

				if(needsImageView(dsType)) {
					combined.total += 1;
					if(bindA.imageView == bindB.imageView) {
						combined.match += 1;
					} else {
						auto viewMatch = match(mt, bindA.imageView, bindB.imageView);
						if(noMatch(viewMatch)) {
							add(combined, viewMatch);
						} else {
							combined.total += 1.f;
						}
					}
				}

				// NOTE: consider image layout? not too relevant I guess
				m.match += eval(combined);
			}
		} else if(dsCat == DescriptorCategory::buffer) {
			auto bindingsA = buffers(a, bindingID);
			auto bindingsB = buffers(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];

				auto bufMatch = match(mt, bindA.buffer, bindB.buffer);
				if(!noMatch(m)) {
					add(bufMatch, bindA.offset, bindB.offset, 0.1);
					add(bufMatch, bindA.range, bindB.range);
					m.match += eval(bufMatch);
				}
			}
		} else if(dsCat == DescriptorCategory::bufferView) {
			auto bindingsA = bufferViews(a, bindingID);
			auto bindingsB = bufferViews(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto bvMatch = match(mt, bindingsA[e].bufferView, bindingsB[e].bufferView);
				m.match += eval(bvMatch);
			}
		} else if(dsCat == DescriptorCategory::accelStruct) {
			auto bindingsA = accelStructs(a, bindingID);
			auto bindingsB = accelStructs(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto asMatch = match(mt, bindingsA[e].accelStruct, bindingsB[e].accelStruct);
				m.match += eval(asMatch);
			}
		} else if(dsCat == DescriptorCategory::inlineUniformBlock) {
			auto bytesA = inlineUniformBlock(a, bindingID);
			auto bytesB = inlineUniformBlock(b, bindingID);
			if(bytesA.size() == bytesB.size() &&
					std::memcmp(bytesA.data(), bytesB.data(), bytesA.size()) == 0) {
				++m.match;
			}
		} else {
			dlg_error("Unsupported descriptor type: {}", u32(dsType));
		}
	}

	// TODO: account in matcher for non-overlapping bindings?
	//   we currently just ignore them but should probably
	//   add them to 'total'

	return m;
}

bool same(span<void*> a, span<void*> b, unsigned offset = 1u) {
	if(a.size() != b.size()) {
		return false;
	}

	if(a.size() <= offset) {
		return true;
	}

	return std::equal(a.begin() + offset, a.end(), b.begin() + offset);
}

// Adds the given stats to the given matcher
void add(MatchVal& m, MatchType mt,
		const ParentCommand::SectionStats& a,
		const ParentCommand::SectionStats& b) {
	auto addMatch = [&](u32 dst, u32 src, float weight = 1.f) {
		m.match += weight * std::min(dst, src); // in range [0, max]
		m.total += weight * std::max(dst, src);
	};

	addMatch(a.numDispatches, b.numDispatches);
	addMatch(a.numDraws, b.numDraws);
	addMatch(a.numRayTraces, b.numRayTraces);
	addMatch(a.numTransfers, b.numTransfers);
	addMatch(a.numSyncCommands, b.numSyncCommands);

	addMatch(a.numTotalCommands, b.numTotalCommands);

	// every pipeline match counts like N commands
	const auto pipeWeight = 10.f;
	m.total += pipeWeight * std::max(a.numPipeBinds, b.numPipeBinds);

	// TODO PERF might be too expensive
	for(auto pipeA = b.boundPipelines; pipeA; pipeA = pipeA->next) {
		float bestMatch = 0.f;
		for(auto pipeB = b.boundPipelines; pipeB; pipeB = pipeB->next) {
			auto pipeMatch = match(mt, pipeA->pipe, pipeB->pipe);
			bestMatch = std::max(bestMatch, eval(pipeMatch));
		}
		m.match += bestMatch;
	}
}

float approxTotalWeight(const ParentCommand& cmd) {
	float ret = 10.f; // wild guess
	ret += cmd.sectionStats().numPipeBinds * 10.f;
	for(auto it = cmd.firstChildParent(); it; it = it->nextParent_) {
		ret += approxTotalWeight(*it);
	}
	return ret;
}

float approxTotalWeight(const FrameSubmission& subm) {
	auto ret = 1.f; // queue
	for(auto& sub : subm.submissions) {
		ret += approxTotalWeight(*sub->commands);
	}
	return ret;
}

LazyMatrixMarch::Result runLMM(u32 width, u32 height,
		LinAllocScope& localMem, LazyMatrixMarch::Matcher matcher) {
	constexpr auto branchThreshold = 0.9f;
	auto customUse = localMem.customUse();
	LazyMatrixMarch lmm(width, height, localMem.tc, matcher, branchThreshold);
	return lmm.run();
}

// For command matching
MatchVal match(const Command& a, const Command& b, MatchType matchType);

// command hierarchy matching
CommandSectionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
		MatchType mt, const ParentCommand& rootA, const ParentCommand& rootB) {
	ZoneScoped;

	// TODO: fast patch for &rootA == &rootB
	// take additional parameter on whether the matches are really needed?
	// in some cases they are not, which would make this a lot cheaper.

	CommandSectionMatch ret;
	ret.a = &rootA;
	ret.b = &rootB;

	// match commands themselves
	ret.match = match(rootA, rootB, mt);
	dlg_assert(valid(ret.match));
	if(ret.match.match <= 0.f) {
		return ret;
	}

#ifdef VIL_COMMAND_CALLSTACKS
	// NOTE WIP: use callstacks for matching.
	// Really hard-reject if they aren't the same?
	// Should probably make this an option, there might be
	// special cases I'm not thinkin of rn.
	if(!same(rootA.stacktrace, rootB.stacktrace)) {
		ret.match = MatchVal::noMatch();
		return ret;
	}
#endif // VIL_COMMAND_CALLSTACKS

	// consider sectionStats for rootMatch.
	auto statsA = rootA.sectionStats();
	auto statsB = rootB.sectionStats();
	add(ret.match, mt, statsA, statsB);
	dlg_assert(valid(ret.match));

	auto numSectionsA = rootA.sectionStats().numChildSections;
	auto numSectionsB = rootB.sectionStats().numChildSections;

	// no child matching to do
	// but make sure to account for the "lost" match values here as well
	if(numSectionsA == 0u || numSectionsB == 0u) {
		for(auto it = rootA.firstChildParent(); it; it = it->nextParent_) {
			ret.match.total += approxTotalWeight(*it);
		}

		for(auto it = rootB.firstChildParent(); it; it = it->nextParent_) {
			ret.match.total += approxTotalWeight(*it);
		}

		return ret;
	}

	// store sections for fast random access below
	auto sectionsA = localMem.alloc<const ParentCommand*>(numSectionsA);
	auto id = 0u;
	for(auto it = rootA.firstChildParent(); it; it = it->nextParent_) {
		sectionsA[id] = it;
		++id;
	}

	auto sectionsB = localMem.alloc<const ParentCommand*>(numSectionsB);
	id = 0u;
	for(auto it = rootB.firstChildParent(); it; it = it->nextParent_) {
		sectionsB[id] = it;
		++id;
	}

	// the resulting matches, filled lazily
	auto evalMatches = localMem.alloc<CommandSectionMatch>(numSectionsA * numSectionsB);

	// our matcher as passed to the matching algorithm below
	auto matchingFunc = [&](u32 i, u32 j) {
		ExtZoneScoped;

		auto& parentA = *sectionsA[i];
		auto& parentB = *sectionsB[j];

		auto& dst = evalMatches[j * numSectionsA + i];
		dlg_assert(!dst.a);

		// make sure that we can re-use the local memory after this
		LinAllocScope localNext(localMem.tc);
		dst = match(retMem, localNext, mt, parentA, parentB);
		return eval(dst.match);

		// NOTE: alternative evaluation
		// Might seem weird at first, but here we'd be maximizing
		// the absolute number of successful matches instead of the rate, which
		// also makes sense.
		// Natively, evaluating the rate only would mean we might prefer
		// a small section with 0.7 match over a gian section with 0.6
		// match which might not be expected.
		// But since we evaluate the "missed weight" below as well,
		// the rate-based-evaluation we do above makes more sense
		// return dst.match.match;
	};

	auto lmmRes = runLMM(numSectionsA, numSectionsB, localMem, matchingFunc);

	ret.children = retMem.alloc<CommandSectionMatch>(lmmRes.matches.size());
	id = 0u;
	auto nextI = 0u;
	auto nextJ = 0u;
	for(auto& match : lmmRes.matches) {
		auto& src = evalMatches[match.j * numSectionsA + match.i];
		dlg_assert(src.a);

		ret.children[id] = src;
		ret.match.match += src.match.match;
		ret.match.total += src.match.total;

		// add approximation of missed weight to ret.match.total
		for(; nextI < match.i; ++nextI) {
			ret.match.total += approxTotalWeight(*sectionsA[nextI]);
		}

		for(; nextJ < match.j; ++nextJ) {
			ret.match.total += approxTotalWeight(*sectionsB[nextJ]);
		}

		++nextI;
		++nextJ;
		++id;
	}

	for(; nextI < numSectionsA; ++nextI) {
		ret.match.total += approxTotalWeight(*sectionsA[nextI]);
	}

	for(; nextJ < numSectionsB; ++nextJ) {
		ret.match.total += approxTotalWeight(*sectionsB[nextJ]);
	}

	return ret;
}

FrameSubmissionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
		MatchType mt, const FrameSubmission& a, const FrameSubmission& b) {
	// TODO WIP: nullptr queue for serialize
	if(a.queue != b.queue && a.queue && b.queue) {
		return {MatchVal::noMatch(), &a, &b, {}};
	}

	FrameSubmissionMatch ret;
	ret.a = &a;
	ret.b = &b;
	// for matching queue
	ret.match.match += 1.f;
	ret.match.total += 1.f;

	// NOTE: add matching for sparse bindings?
	//   This probably does not make sense though since no application
	//   will submit similar sparse bindings over and over again so matching
	//   is somewhat pointless

	if(a.submissions.size() == 0u && b.submissions.size() == 0u) {
		return ret;
	} else if(a.submissions.size() == 0u || b.submissions.size() == 0u) {
		for(auto& subm : a.submissions) {
			ret.match.total += approxTotalWeight(*subm->commands);
		}
		for(auto& subm : b.submissions) {
			ret.match.total += approxTotalWeight(*subm->commands);
		}
		return ret;
	}

	// the resulting matches, filled lazily
	auto evalMatches = localMem.alloc<CommandRecordMatch>(
		a.submissions.size() * b.submissions.size());

	auto matchingFunc = [&](u32 i, u32 j) {
		LinAllocScope nextLocalMem(localMem.tc);

		// TODO: consider name and other properties of the records?

		auto& recA = *a.submissions[i];
		auto& recB = *b.submissions[j];
		auto ret = match(retMem, nextLocalMem,
			mt, *recA.commands, *recB.commands);

		evalMatches[j * a.submissions.size() + i].a = &recA;
		evalMatches[j * a.submissions.size() + i].b = &recB;
		evalMatches[j * a.submissions.size() + i].matches = retMem.alloc<CommandSectionMatch>(1u);
		evalMatches[j * a.submissions.size() + i].matches[0] = ret;
		evalMatches[j * a.submissions.size() + i].match = ret.match;

		return eval(ret.match);
	};

	auto lmmRes = runLMM(a.submissions.size(), b.submissions.size(),
		localMem, matchingFunc);

	// fill ret
	ret.matches = retMem.alloc<CommandRecordMatch>(lmmRes.matches.size());
	auto id = 0u;
	auto nextI = 0u;
	auto nextJ = 0u;
	for(auto& match : lmmRes.matches) {
		auto& src = evalMatches[match.j * a.submissions.size() + match.i];
		ret.matches[id] = src;
		ret.match.match += src.match.match;
		ret.match.total += src.match.total;

		// add approximation of missed weight to ret.match.total
		for(; nextI < match.i; ++nextI) {
			ret.match.total += approxTotalWeight(*a.submissions[nextI]->commands);
		}

		for(; nextJ < match.j; ++nextJ) {
			ret.match.total += approxTotalWeight(*b.submissions[nextJ]->commands);
		}

		++nextI;
		++nextJ;
		++id;
	}

	// add approximation of missed weight to ret.match.total
	for(; nextI < a.submissions.size(); ++nextI) {
		ret.match.total += approxTotalWeight(*a.submissions[nextI]->commands);
	}

	for(; nextJ < b.submissions.size(); ++nextJ) {
		ret.match.total += approxTotalWeight(*b.submissions[nextJ]->commands);
	}

	return ret;
}

FrameMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
		MatchType mt, span<const FrameSubmission> a, span<const FrameSubmission> b) {
	ZoneScoped;

	if(a.empty() && b.empty()) {
		// empty actually means full match
		return {};
	} else if(a.empty() || b.empty()) {
		return {MatchVal::noMatch(), {}};
	}

	// the resulting matches, filled lazily
	auto evalMatches = localMem.alloc<FrameSubmissionMatch>(
		a.size() * b.size());

	auto matchingFunc = [&](u32 i, u32 j) {
		LinAllocScope nextLocalMem(localMem.tc);
		auto ret = match(retMem, nextLocalMem, mt, a[i], b[j]);
		evalMatches[j * a.size() + i] = ret;
		return eval(ret.match);
	};

	auto lmmRes = runLMM(a.size(), b.size(), localMem, matchingFunc);

	// fill return info
	FrameMatch ret;
	ret.matches = retMem.alloc<FrameSubmissionMatch>(lmmRes.matches.size());
	auto id = 0u;
	auto nextI = 0u;
	auto nextJ = 0u;
	for(auto& match : lmmRes.matches) {
		auto& src = evalMatches[match.j * a.size() + match.i];
		ret.matches[id] = src;
		ret.match.match += src.match.match;
		ret.match.total += src.match.total;

		// add approximation of missed weight to ret.match.total
		for(; nextI < match.i; ++nextI) {
			ret.match.total += approxTotalWeight(a[nextI]);
		}

		for(; nextJ < match.j; ++nextJ) {
			ret.match.total += approxTotalWeight(b[nextJ]);
		}

		++nextI;
		++nextJ;
		++id;
	}

	for(; nextI < a.size(); ++nextI) {
		ret.match.total += approxTotalWeight(a[nextI]);
	}

	for(; nextJ < b.size(); ++nextJ) {
		ret.match.total += approxTotalWeight(b[nextJ]);
	}

	return ret;
}

// finding
using RelIDPair = std::pair<const std::string_view, u32>;
using RelIDMap = std::unordered_map<std::string_view, u32,
		std::hash<std::string_view>,
		std::equal_to<std::string_view>,
		LinearScopedAllocator<RelIDPair>>;

u32 evaluateRelID(RelIDMap& ids, const Command& start, const Command& dst) {
	auto* cmd = &start;
	while(cmd) {
		auto it = ids.find(cmd->nameDesc());
		if(it == ids.end()) {
			it = ids.emplace(cmd->nameDesc(), 0u).first;
		} else {
			++it->second;
		}

		if(cmd == &dst) {
			return it->second;
		}

		cmd = cmd->next;
	}

	dlg_error("unreachable");
	return 0xFFFFFFFFu;
}

FindResult find(MatchType mt, const Command& srcParent, const Command& src,
		const Command& dstParent, span<const Command*> dst,
		const CommandDescriptorSnapshot& dstDsState, float threshold) {
	ZoneScoped;

	dlg_assert_or(!dst.empty(), return {});
	dlg_assert(srcParent.isChild(src));

	std::vector<const Command*> bestCmds;
	float bestMatch = threshold;

	struct EvaluatedRelativeIDs {
		u32 dst;
		u32 best;
	};

	std::optional<EvaluatedRelativeIDs> relIDs;

	ThreadMemScope tms;
	RelIDMap relIDMap{tms};

	for(auto it = &src; it; it = it->next) {
		auto m = match(*it, *dst[0], mt);
		auto em = eval(m);

		// dlg_trace("em: {} {} - {}", em, it->toString(), dst[0]->toString());

		// NOTE: early-continue on less-than-best here incorrect for
		// non-parent commands as we might increase the matching value below.
		if(em == 0.f || (em < bestMatch && dst.size() > 1)) {
			continue;
		}

#ifdef VIL_COMMAND_CALLSTACKS
		// NOTE WIP: use callstacks for matching.
		// Really hard-reject if they aren't the same?
		// Should probably make this an option, there might be
		// special cases I'm not thinkin of rn.
		if(!same(it->stacktrace, dst[0]->stacktrace)) {
			continue;
		}
#endif // VIL_COMMAND_CALLSTACKS

		std::vector<const Command*> currCmds {it};
		float childMatch = 1.f;

		if(dst.size() > 1) {
			dlg_assert(it->children());
			auto newThresh = bestMatch / em;
			auto restResult = find(mt, *it, *it->children(),
				*dst[0], dst.subspan(1), dstDsState, newThresh);
			if(restResult.hierarchy.empty()) {
				// no candidate found
				continue;
			}

			auto& rest = restResult.hierarchy;
			currCmds.insert(currCmds.end(), rest.begin(), rest.end());
			childMatch = restResult.match;

			// TODO: replace dynamic_cast with some 'isStateCmd(const Command&)'
			//  check that simply checks for category (draw | dispatch | traceRays)
		} else if(auto srcCmd = dynamic_cast<const StateCmdBase*>(it); srcCmd) {
			// match descriptors, if any
			// TODO: only consider descriptors statically used by pipeline
			// NOTE: the `min(dsCount, ...)` is used defensively here to
			//   account for extensions that mess with bound-descriptor
			//   requirements, e.g. push descriptors.
			auto* dstCmd = dynamic_cast<const StateCmdBase*>(dst[0]);
			dlg_assert_or(dstCmd, continue);

			auto* srcPipe = srcCmd->boundPipe();
			auto* dstPipe = dstCmd->boundPipe();

			// hard guarantee from matching: we never match state commands with
			// incompatible pipes. Would lead to issues later on, e.g.
			// gui code might assume this as well
			dlg_assert_or(!conflicting(*srcPipe->layout, *dstPipe->layout), continue);

			auto srcDescriptors = srcCmd->boundDescriptors().descriptorSets;
			auto dstDescriptors = dstCmd->boundDescriptors().descriptorSets;

			auto dsCount = dstPipe->layout->descriptors.size();
			auto srcBound = srcDescriptors.first(std::min(dsCount, srcDescriptors.size()));
			auto dstBound = dstDescriptors.first(std::min(dsCount, dstDescriptors.size()));

			if(!dstBound.empty() || !srcBound.empty()) {
				// TODO: consider dynamic offsets?
				for(auto i = 0u; i < std::min(srcBound.size(), dstBound.size()); ++i) {
					if(!srcBound[i].dsEntry || !dstBound[i].dsEntry) {
						// TODO: not sure if this can happen. Do sets
						// that are statically not used by pipeline
						// have to be bound?
						dlg_warn("ds not bound? shouldn't happen");
						continue;
					}

					// We cannot rely on the src record being valid here
					// srcLock will hold a look on the ds pool while
					// dstLock below will hold a lock on a specific cow (valid
					// lock order).
					auto [srcDs, srcLock] = tryAccess(srcBound[i]);
					if(!srcDs) {
						continue;
					}

					auto dstDsCow = dstDsState.states.find(dstBound[i].dsEntry);
					// TODO: we might not find it here due to the new
					// descriptor set capturing rework.
					if(dstDsCow == dstDsState.states.end()) {
						continue;
					}

					auto [dstDs, dstLock] = access(*dstDsCow->second);

					auto res = vil::match(mt, *srcDs, dstDs);
					m.match += res.match;
					m.total += res.total;
				}

				// auto dsMatch = float(match) / std::max(srcBound.size(), dstBound.size());
				// dlg_trace("match: {}, dsMatch: {}", m, dsMatch);
			}
		}

		em = eval(m) * childMatch;

		if(em == 0.f || em < bestMatch) {
			continue;
		} else if(em == bestMatch && !bestCmds.empty()) {
			// When the match values of two commands are equal (which isn't
			// too unlikely, the default Command::match implementation
			// simply returns 1.f when the types are equal), choose
			// simply by order in current hierarchy level.
			if(!relIDs) {
				auto& rids = relIDs.emplace();
				RelIDMap tmpMap{tms};
				rids.dst = evaluateRelID(tmpMap,
					*dstParent.children(), *dst[0]);

				rids.best = evaluateRelID(relIDMap,
					*srcParent.children(), *bestCmds[0]);
			}

			// use bestCmds[0]->next instead of srcParent.children() and reuse relIDMap
			// to not iterate over *all* commands again.
			auto currRelID = evaluateRelID(relIDMap,
				*bestCmds[0]->next, *currCmds[0]);
			if(std::abs(int(currRelID) - int(relIDs->dst)) >=
					std::abs(int(relIDs->best) - int(relIDs->dst))) {
				continue;
			}

			relIDs->best = currRelID;
		}

		bestCmds.clear();
		bestCmds = std::move(currCmds);
		bestMatch = em;
	}

	return {bestCmds, bestMatch};
}

FindResult find(MatchType mt, const ParentCommand& srcRoot,
		span<const Command*> dstHierarchyToFind,
		const CommandDescriptorSnapshot& dstDescriptors, float threshold) {
	// empty hierarchy
	if(!srcRoot.children()) {
		return {};
	}

	// otherwise only the root command would be selected, makes no sense
	dlg_assert(dstHierarchyToFind.size() >= 2);
	dlg_assert(dynamic_cast<const ParentCommand*>(dstHierarchyToFind[0]));

	auto ret = find(mt, srcRoot, *srcRoot.children(),
		*dstHierarchyToFind[0], dstHierarchyToFind.subspan(1),
		dstDescriptors, threshold);
	if(!ret.hierarchy.empty()) {
		ret.hierarchy.insert(ret.hierarchy.begin(), &srcRoot);
	}

	return ret;
}

MatchVal match(MatchType, const VkBufferCopy2KHR& a, const VkBufferCopy2KHR& b) {
	MatchVal m;
	add(m, a.size, b.size);
	add(m, a.srcOffset, b.srcOffset, 0.2);
	add(m, a.dstOffset, b.dstOffset, 0.2);
	return m;
}

MatchVal match(MatchType, const VkImageCopy2KHR& a, const VkImageCopy2KHR& b) {
	MatchVal m;
	addMemcmp(m, a.dstOffset, b.dstOffset);
	addMemcmp(m, a.srcOffset, b.srcOffset);
	addMemcmp(m, a.extent, b.extent);
	add(m, a.dstSubresource, b.dstSubresource);
	add(m, a.srcSubresource, b.srcSubresource);
	return m;
}

MatchVal match(MatchType, const VkImageBlit2KHR& a, const VkImageBlit2KHR& b) {
	MatchVal m;
	add(m, a.srcSubresource, b.srcSubresource);
	add(m, a.dstSubresource, b.dstSubresource);
	addMemcmp(m, a.srcOffsets, b.srcOffsets);
	addMemcmp(m, a.dstOffsets, b.dstOffsets);
	return m;
}

MatchVal match(MatchType, const VkImageResolve2KHR& a, const VkImageResolve2KHR& b) {
	MatchVal m;
	add(m, a.srcSubresource, b.srcSubresource);
	add(m, a.dstSubresource, b.dstSubresource);
	addMemcmp(m, a.srcOffset, b.srcOffset);
	addMemcmp(m, a.dstOffset, b.dstOffset);
	addMemcmp(m, a.extent, b.extent);
	return m;
}

MatchVal match(MatchType, const VkBufferImageCopy2KHR& a, const VkBufferImageCopy2KHR& b) {
	MatchVal m;
	add(m, a.bufferImageHeight, b.bufferImageHeight);
	add(m, a.bufferOffset, b.bufferOffset);
	add(m, a.bufferRowLength, b.bufferRowLength);
	addMemcmp(m, a.imageOffset, b.imageOffset);
	addMemcmp(m, a.imageExtent, b.imageExtent);
	add(m, a.imageSubresource, b.imageSubresource);
	return m;
}

MatchVal match(MatchType, const VkClearAttachment& a, const VkClearAttachment& b) {
	if(a.aspectMask != b.aspectMask) {
		return MatchVal::noMatch();
	}

	auto sameCV = false;

	// TODO union comparison is sketchy
	if(a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		if(a.colorAttachment != b.colorAttachment) {
			return MatchVal::noMatch();
		}
		// NOTE: stricly speaking we would have to check the format here
		// and compare based on that
		sameCV = std::memcmp(&a.clearValue.color.uint32, &b.clearValue.color.uint32,
			sizeof(a.clearValue.color.uint32)) == 0;
	} else {
		sameCV = std::memcmp(&a.clearValue.depthStencil, &b.clearValue.depthStencil,
			sizeof(a.clearValue.depthStencil)) == 0;
	}

	return sameCV ? MatchVal{3.f, 3.f} : MatchVal::noMatch();
}

MatchVal match(MatchType, const VkClearRect& a, const VkClearRect& b) {
	MatchVal m;
	add(m, a.rect.offset.x, b.rect.offset.x);
	add(m, a.rect.offset.y, b.rect.offset.y);
	add(m, a.rect.extent.width, b.rect.extent.width);
	add(m, a.rect.extent.height, b.rect.extent.height);
	add(m, a.baseArrayLayer, b.baseArrayLayer);
	add(m, a.layerCount, b.layerCount);
	return m;
}

MatchVal match(MatchType mt, const BoundVertexBuffer& a, const BoundVertexBuffer& b) {
	auto m = match(mt, a.buffer, b.buffer);
	if(m.match == 0.f || m.total == -1.f) {
		return m;
	}
	add(m, a.offset, b.offset, 0.1);
	return m;
}

template<typename ResourceBarrier>
bool equalOwnershipTransition(const ResourceBarrier& a, const ResourceBarrier& b) {
	bool queueTransferA =
		a.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		a.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		a.srcQueueFamilyIndex != a.dstQueueFamilyIndex;
	bool queueTransferB =
		b.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		b.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		b.srcQueueFamilyIndex != b.dstQueueFamilyIndex;

	if(queueTransferA || queueTransferB) {
		return queueTransferA == queueTransferB &&
				a.srcQueueFamilyIndex == b.srcQueueFamilyIndex &&
				a.dstQueueFamilyIndex == b.dstQueueFamilyIndex;
	}

	return true;
}

// dummy for validExpression below
template<typename B> using SrcStageMaskMember = decltype(B::srcStageMask);

template<typename ImageBarrier>
MatchVal matchImgBarrier(MatchType mt, const ImageBarrier& a,
		const Image* imgA, const ImageBarrier& b, const Image* imgB) {
	if(!equalOwnershipTransition(a, b)) {
		return MatchVal::noMatch();
	}

	MatchVal m = match(mt, imgA, imgB);
	if(noMatch(m)) {
		return MatchVal::noMatch();
	}

	if constexpr(validExpression<SrcStageMaskMember, ImageBarrier>) {
		addBits(m, a.srcStageMask, b.srcStageMask);
		addBits(m, a.dstStageMask, b.dstStageMask);
	}

	addBits(m, a.srcAccessMask, b.srcAccessMask);
	addBits(m, a.dstAccessMask, b.dstAccessMask);
	add(m, a.oldLayout, b.oldLayout);
	add(m, a.newLayout, b.newLayout);
	add(m, a.subresourceRange, b.subresourceRange);

	dlg_assert(valid(m));

	return m;
}

template<typename BufferBarrier>
MatchVal matchBufBarrier(MatchType mt, const BufferBarrier& a,
		const Buffer* bufA, const BufferBarrier& b, const Buffer* bufB) {
	if(!equalOwnershipTransition(a, b)) {
		return MatchVal::noMatch();
	}

	MatchVal m = match(mt, bufA, bufB);
	if(noMatch(m)) {
		return MatchVal::noMatch();
	}

	if constexpr(validExpression<SrcStageMaskMember, BufferBarrier>) {
		addBits(m, a.srcStageMask, b.srcStageMask);
		addBits(m, a.dstStageMask, b.dstStageMask);
	}

	addBits(m, a.srcAccessMask, b.srcAccessMask);
	addBits(m, a.dstAccessMask, b.dstAccessMask);
	add(m, a.offset, b.offset);
	add(m, a.size, b.size);

	dlg_assert(valid(m));

	return m;
}

MatchVal match(MatchType mt, const VkMemoryBarrier& a, const VkMemoryBarrier& b) {
	(void) mt;
	MatchVal m;
	addBits(m, a.dstAccessMask, b.dstAccessMask);
	addBits(m, a.srcAccessMask, b.srcAccessMask);
	return m;
}

MatchVal match(MatchType mt, const VkMemoryBarrier2& a, const VkMemoryBarrier2& b) {
	(void) mt;
	MatchVal m;
	addBits(m, a.dstAccessMask, b.dstAccessMask);
	addBits(m, a.srcAccessMask, b.srcAccessMask);
	addBits(m, a.srcStageMask, b.srcStageMask);
	addBits(m, a.dstStageMask, b.dstStageMask);
	return m;
}

template<typename F>
void addSpanOrderedStrict(MatchVal& m, u64 countA, u64 countB,
		F callback, float weight = 1.0) {

	m.total += weight;

	if(countA == 0u && countB == 0u) {
		m.match += weight;
		return;
	}

	MatchVal accum {};
	for(auto i = 0u; i < std::min(countA, countB); ++i) {
		auto res = callback(i, i);
		if(noMatch(res)) {
			accum.total += 1.0;
		} else {
			accum.match += res.match;
			accum.total += res.total;
		}
	}

	dlg_assert(valid(accum));

	// TODO: maybe better to make weight also dependent on size?
	// could add flag/param for that behavior.
	m.match += weight * eval(accum);
}

template<typename Cmd>
void addImgBufBarriers(MatchVal& m, MatchType mt, const Cmd& a, const Cmd& b) {
	dlg_assert(a.bufBarriers.size() == a.buffers.size());
	dlg_assert(b.bufBarriers.size() == b.buffers.size());
	auto cbBuf = [&](auto ia, auto ib) {
		return matchBufBarrier(mt, a.bufBarriers[ia], a.buffers[ia],
			b.bufBarriers[ib], b.buffers[ib]);
	};

	dlg_assert(a.imgBarriers.size() == a.images.size());
	dlg_assert(b.imgBarriers.size() == b.images.size());
	auto cbImg = [&](auto ia, auto ib) {
		return matchImgBarrier(mt, a.imgBarriers[ia], a.images[ia],
			b.imgBarriers[ib], b.images[ib]);
	};

	addSpanOrderedStrict(m, a.bufBarriers.size(), b.bufBarriers.size(), cbBuf);
	addSpanOrderedStrict(m, a.imgBarriers.size(), b.imgBarriers.size(), cbImg);
}

MatchVal doMatch(MatchType mt, const BarrierCmdBase& a, const BarrierCmdBase& b) {
	MatchVal m;

	// NOTE: we match hard on srcStageMask and dstStageMask now, otherwise
	// we get way too many match candidates. With this formulation, we also
	// need at least one common resource
	// add(m, this->srcStageMask, cmd.srcStageMask);
	// add(m, this->dstStageMask, cmd.dstStageMask);
	if(a.srcStageMask != b.srcStageMask ||
			a.dstStageMask != b.dstStageMask) {
		return MatchVal::noMatch();
	}

	// NOTE: could use unordered here, see node 2668
	addSpanOrderedStrict(m, mt, a.memBarriers, b.memBarriers);
	addImgBufBarriers(m, mt, a, b);

	dlg_assert(m.match <= m.total);
	return m;
}

MatchVal doMatch(MatchType mt, const Barrier2CmdBase& a, const Barrier2CmdBase& b) {
	MatchVal m;

	// NOTE: could use unordered here, see node 2668
	addSpanOrderedStrict(m, mt, a.memBarriers, b.memBarriers);
	addImgBufBarriers(m, mt, a, b);

	if(m.match == 0.f) {
		return MatchVal::noMatch();
	}

	add(m, a.flags, b.flags);

	dlg_assert(m.match <= m.total);
	return m;
}

MatchVal matchState(MatchType matchType, const StateCmdBase& a, const StateCmdBase& b) {
	auto m = match(matchType, a.boundPipe(), b.boundPipe());

	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(noMatch(m)) {
		return m;
	}

	// same pipeline for a state cmd is a pretty important indicator
	m.total += 5.f;
	m.match += 5.f;

	// this case should have already been caught in the incomptabile-pipeline
	// case above
	dlg_assert_or(a.boundPipe() && b.boundPipe(), return MatchVal::noMatch());
	dlg_assert_or(a.boundPipe()->layout->pushConstants.size() ==
			b.boundPipe()->layout->pushConstants.size(),
			return MatchVal::noMatch());

	for(auto& pcr : a.boundPipe()->layout->pushConstants) {
		// TODO: these asserts can trigger if parts of the push constant
		// range was left undefined. It might not be used by the shader
		// anyways. Not sure how to fix.
		dlg_assertl_or(dlg_level_warn,
			pcr.offset + pcr.size <= a.boundPushConstants().data.size(), continue);
		dlg_assertl_or(dlg_level_warn,
			pcr.offset + pcr.size <= b.boundPushConstants().data.size(), continue);

		auto pcrWeight = 1.f; // std::min(pcr.size / 4u, 4u);
		m.total += pcrWeight;
		if(std::memcmp(&a.boundPushConstants().data[pcr.offset],
				&b.boundPushConstants().data[pcr.offset], pcr.size) == 0u) {
			m.match += pcrWeight;
		}
	}

	return m;
}

MatchVal doMatch(MatchType matchType, const DrawCmdBase& a, const DrawCmdBase& b, bool indexed) {
	auto m = matchState(matchType, a, b);
	if(noMatch(m)) {
		return MatchVal::noMatch();
	}

	for(auto i = 0u; i < a.state->pipe->vertexBindings.size(); ++i) {
		dlg_assert_or(i < a.state->vertices.size(), break);
		dlg_assert_or(i < b.state->vertices.size(), break);

		addNonNull(m, a.state->vertices[i].buffer, b.state->vertices[i].buffer);

		// Low weight on offset here, it can change frequently for dynamic
		// draw data. But the same buffer is a good indicator for similar
		// commands
		add(m, a.state->vertices[i].offset, b.state->vertices[i].offset, 0.1);
	}

	if(indexed) {
		addNonNull(m, a.state->indices.buffer, b.state->indices.buffer);
		add(m, a.state->indices.offset, b.state->indices.offset, 0.1);

		// different index types is an indicator for fundamentally different
		// commands.
		if(a.state->indices.type != b.state->indices.type) {
			return MatchVal::noMatch();
		}
	}

	// - we consider the bound descriptors somewhere else since they might
	//   already have been unset from the command
	// - we don't consider the render pass instance here since that should
	//   already have been taken into account via the parent commands
	// TODO: consider dynamic state?

	return m;
}

MatchVal doMatch(MatchType mt, const TraceRaysCmdBase& a, const TraceRaysCmdBase& b) {
	auto m = matchState(mt, a, b);
	if(noMatch(m)) {
		return MatchVal::noMatch();
	}

	// - we consider the bound descriptors somewhere else since they might
	//   already have been unset from the command

	return m;
}

MatchVal match(MatchType mt, const WaitEventsCmd& a, const WaitEventsCmd& b) {
	auto ret = doMatch(mt, a, b);
	addSpanOrderedStrict(ret, mt, a.events, b.events);
	return ret;
}

MatchVal match(MatchType mt, const BarrierCmd& a, const BarrierCmd& b) {
	auto ret = doMatch(mt, a, b);
	if(ret.match == 0.f || ret.total == -1.f) {
		return ret;
	}

	add(ret, a.dependencyFlags, b.dependencyFlags);
	return ret;
}

MatchVal match(MatchType mt, const WaitEvents2Cmd& a, const WaitEvents2Cmd& b) {
	auto ret = doMatch(mt, a, b);
	if(ret.match == 0.f || ret.total == -1.f) {
		return ret;
	}

	addSpanOrderedStrict(ret, mt, a.events, b.events);
	return ret;
}

MatchVal match(MatchType mt, const Barrier2Cmd& a, const Barrier2Cmd& b) {
	return doMatch(mt, a, b);
}

MatchVal match(MatchType mt, const DrawCmd& a, const DrawCmd& b) {
	// Hard matching on {indexCount, firstIndex, vertexOffset} since that's
	// dependent on the rendered mesh.
	// Soft matching on instancing parameters since renderers might batch dynamically,
	// making this variable at runtime (but still obviously the same command).
	if(a.vertexCount != b.vertexCount || a.firstVertex != b.firstVertex) {
		return MatchVal::noMatch();
	}

	auto ret = doMatch(mt, a, b, false);
	if(ret.total == -1.f) {
		return ret;
	}

	ret.total += 5.f;
	ret.match += 5.f;
	add(ret, a.instanceCount, b.instanceCount, 3.f);
	add(ret, a.firstInstance, b.firstInstance, 3.f);
	return ret;
}

MatchVal match(MatchType mt, const DrawIndirectCmd& a, const DrawIndirectCmd& b) {
	// hard matching on those; differences would indicate a totally
	// different command structure.
	if(a.indexed != b.indexed || a.stride != b.stride) {
		return MatchVal::noMatch();
	}

	auto ret = doMatch(mt, a, b, b.indexed);
	if(ret.total == -1.f) {
		return ret;
	}

	ret.total += 5.f;
	ret.match += 5.f;

	addNonNull(ret, a.buffer, b.buffer);

	// we don't hard-match on drawCount since architectures that choose
	// this dynamically per-frame (e.g. for culling) are common
	add(ret, a.drawCount, b.drawCount);
	add(ret, a.offset, b.offset, 0.2);
	return ret;
}

MatchVal match(MatchType mt, const DrawIndexedCmd& a, const DrawIndexedCmd& b) {
	// Hard matching on {indexCount, firstIndex, vertexOffset} since that's
	// dependent on the rendered mesh.
	// Soft matching on instancing parameters since renderers might batch dynamically,
	// making this variable at runtime (but still obviously the same command).
	if(a.indexCount != b.indexCount ||
			a.firstIndex != b.firstIndex ||
			a.vertexOffset != b.vertexOffset) {
		return MatchVal::noMatch();
	}

	auto ret = doMatch(mt, a, b, true);
	if(ret.total == -1.f) {
		return ret;
	}

	ret.total += 5.f;
	ret.match += 5.f;

	add(ret, a.instanceCount, b.instanceCount, 3.f);
	add(ret, a.firstInstance, b.firstInstance, 3.f);
	return ret;
}

MatchVal match(MatchType mt, const DrawIndirectCountCmd& a, const DrawIndirectCountCmd& b) {
	// hard matching on those; differences would indicate a totally
	// different command structure.
	if(a.indexed != b.indexed || a.stride != b.stride) {
		return MatchVal::noMatch();
	}

	auto ret = doMatch(mt, a, b, b.indexed);
	if(ret.total == -1.f) {
		return ret;
	}

	ret.match += 2.0;
	ret.total += 2.0;

	addNonNull(ret, a.buffer, b.buffer);
	addNonNull(ret, a.countBuffer, b.countBuffer);

	// we don't hard-match on maxDrawCount since architectures that choose
	// this dynamically per-frame (e.g. for culling) are common
	add(ret, a.maxDrawCount, b.maxDrawCount);
	add(ret, a.countBufferOffset, b.countBufferOffset, 0.2);
	add(ret, a.offset, b.offset, 0.2);
	return ret;
}

MatchVal match(MatchType mt, const DrawMultiCmd& a, const DrawMultiCmd& b) {
	auto ret = doMatch(mt, a,b, false);
	if(ret.total == -1.f) {
		return ret;
	}

	add(ret, a.instanceCount, b.instanceCount);
	add(ret, a.firstInstance, b.firstInstance);
	add(ret, a.stride, b.stride);

	// NOTE: not sure. Could also do unordered
	addSpanOrderedStrict(ret, mt, a.vertexInfos, b.vertexInfos);
	return ret;
}

MatchVal match(MatchType mt, const DrawMultiIndexedCmd& a, const DrawMultiIndexedCmd& b) {
	auto ret = doMatch(mt, a, b, false);
	if(ret.total == -1.f) {
		return ret;
	}

	add(ret, a.instanceCount, b.instanceCount);
	add(ret, a.firstInstance, b.firstInstance);
	add(ret, a.stride, b.stride);

	// NOTE: not sure. Could also do unordered
	addSpanOrderedStrict(ret, mt, a.indexInfos, b.indexInfos);
	return ret;
}

MatchVal match(MatchType mt, const BindVertexBuffersCmd& a, const BindVertexBuffersCmd& b) {
	if(a.firstBinding != b.firstBinding) {
		return MatchVal::noMatch();
	}

	MatchVal ret;
	ret.match += 2.0;
	ret.total += 2.0;

	addSpanOrderedStrict(ret, mt, a.buffers, b.buffers);
	return ret;
}

MatchVal match(MatchType mt, const BindIndexBufferCmd& a, const BindIndexBufferCmd& b) {
	auto ret = match(mt, a.buffer, b.buffer);
	if(ret.total == -1.f || ret.match == 0.f) {
		return ret;
	}

	if(a.indexType != b.indexType) {
		return MatchVal::noMatch();
	}

	ret.match += 1.0;
	ret.total += 1.0;
	add(ret, a.offset, b.offset, 0.2);

	return ret;
}

MatchVal match(MatchType, const BindDescriptorSetCmd& a, const BindDescriptorSetCmd& b) {
	dlg_assert_or(a.pipeLayout, return MatchVal::noMatch());
	dlg_assert_or(b.pipeLayout, return MatchVal::noMatch());

	if(a.firstSet != b.firstSet ||
			// NOTE: could relax this, only needing compatibility up
			// to (and including) the bound sets
			conflicting(*a.pipeLayout, *b.pipeLayout) ||
			a.pipeBindPoint != b.pipeBindPoint) {
		return MatchVal::noMatch();
	}

	// NOTE: evaluating the used descriptor sets or dynamic offsets
	// is likely of no use as they are too instable.

	MatchVal ret;
	ret.total += 3.f;
	ret.match += 3.f;
	return ret;
}

MatchVal match(MatchType mt, const BindPipelineCmd& a, const BindPipelineCmd& b) {
	if(a.bindPoint != b.bindPoint) {
		return MatchVal::noMatch();
	}

	return match(mt, a.pipe, b.pipe);
}

MatchVal doMatch(MatchType mt, const DispatchCmdBase& a, const DispatchCmdBase& b) {
	auto m = matchState(mt, a, b);
	if(noMatch(m)) {
		return MatchVal::noMatch();
	}

	// - we consider the bound descriptors somewhere else since they might
	//   already have been unset from the command

	return m;
}

MatchVal match(MatchType mt, const DispatchCmd& a, const DispatchCmd& b) {
	auto ret = doMatch(mt, a, b);
	if(ret.match == 0.f || ret.total == -1.f) {
		return ret;
	}

	// we don't hard-match on them since this may change for per-frame
	// varying workloads (in comparison to draw parameters, which rarely
	// change for per-frame stuff). The higher the dimension, the more unlikely
	// this gets though.
	add(ret, a.groupsX, b.groupsX, 2.0);
	add(ret, a.groupsY, b.groupsY, 4.0);
	add(ret, a.groupsZ, b.groupsZ, 6.0);
	return ret;
}

MatchVal match(MatchType mt, const DispatchIndirectCmd& a, const DispatchIndirectCmd& b) {
	auto ret = doMatch(mt, a, b);
	if(ret.total == -1.f) {
		return ret;
	}

	addNonNull(ret, a.buffer, b.buffer);
	add(ret, a.offset, b.offset, 0.1);
	return ret;
}

MatchVal match(MatchType mt, const DispatchBaseCmd& a, const DispatchBaseCmd& b) {
	auto ret = doMatch(mt, a, b);
	if(ret.total == -1.f) {
		return ret;
	}

	// we don't hard-match on them since this may change for per-frame
	// varying workloads (in comparison to draw parameters, which rarely
	// change for per-frame stuff). The higher the dimension, the more unlikely
	// this gets though.
	add(ret, a.groupsX, b.groupsX, 2.0);
	add(ret, a.groupsY, b.groupsY, 4.0);
	add(ret, a.groupsZ, b.groupsZ, 8.0);

	add(ret, a.baseGroupX, b.baseGroupX, 2.0);
	add(ret, a.baseGroupY, b.baseGroupY, 4.0);
	add(ret, a.baseGroupZ, b.baseGroupZ, 8.0);
	return ret;
}

MatchVal match(MatchType mt, const BeginRenderPassCmd& a, const BeginRenderPassCmd& b) {
	// Match render pass description.
	if(!a.rp || !b.rp || !equal(a.rp->desc, b.rp->desc)) {
		return MatchVal::noMatch();
	}

	// High base match probability since the RenderPasses matched.
	MatchVal m;
	m.total += 4.f;
	m.match += 4.f;

	// match attachments
	if(a.attachments.size() != b.attachments.size()) {
		return MatchVal::noMatch();
	}

	for(auto i = 0u; i < a.attachments.size(); ++i) {
		auto attMatch = match(mt, a.attachments[i], b.attachments[i]);
		if(noMatch(attMatch)) {
			m.total += 10.f;
		} else {
			m.total += attMatch.total;
			m.match += attMatch.match;
		}
	}

	// TODO: consider render area, clearValues?
	// both might be very dynamic (render area e.g. for dynamic resolution scaling)

	return m;
}

MatchVal match(MatchType, const NextSubpassCmd& a, const NextSubpassCmd& b) {
	// we don't need to consider surrounding RenderPass, that is already
	// considered when matching parent
	return a.subpassID == b.subpassID ? MatchVal{1.f, 1.f} : MatchVal::noMatch();
}

MatchVal match(MatchType, const FirstSubpassCmd& a, const FirstSubpassCmd& b) {
	(void) a;
	(void) b;
	return MatchVal{1.f, 1.f};
}

MatchVal match(MatchType, const RootCommand& a, const RootCommand& b) {
	(void) a;
	(void) b;
	return MatchVal{1.f, 1.f};
}

// NOTE: for copy commands, src/dst layouts seem irrelevant but it's unlikely
// they change between semantically equal commands so we give them full weight.

MatchVal match(MatchType mt, const CopyImageCmd& a, const CopyImageCmd& b) {
	auto mDst = match(mt, a.dst, b.dst);
	auto mSrc = match(mt, a.src, b.src);
	if(noMatch(mDst) || noMatch(mSrc)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	add(m, mDst);
	add(m, mSrc);
	add(m, a.srcLayout, b.srcLayout);
	add(m, a.dstLayout, b.dstLayout);
	addSpanOrderedStrict(m, mt, a.copies, b.copies);

	return m;
}

MatchVal match(MatchType mt, const CopyBufferToImageCmd& a, const CopyBufferToImageCmd& b) {
	auto mDst = match(mt, a.dst, b.dst);
	auto mSrc = match(mt, a.src, b.src);
	if(noMatch(mDst) || noMatch(mSrc)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	add(m, mDst);
	add(m, mSrc);
	add(m, a.dstLayout, b.dstLayout);
	addSpanOrderedStrict(m, mt, a.copies, b.copies);

	return m;
}

MatchVal match(MatchType mt, const CopyImageToBufferCmd& a, const CopyImageToBufferCmd& b) {
	auto mDst = match(mt, a.dst, b.dst);
	auto mSrc = match(mt, a.src, b.src);
	if(noMatch(mDst) || noMatch(mSrc)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	add(m, mDst);
	add(m, mSrc);
	add(m, a.srcLayout, b.srcLayout);
	addSpanOrderedStrict(m, mt, a.copies, b.copies);

	return m;
}

MatchVal match(MatchType mt, const BlitImageCmd& a, const BlitImageCmd& b) {
	auto mDst = match(mt, a.dst, b.dst);
	auto mSrc = match(mt, a.src, b.src);
	if(noMatch(mDst) || noMatch(mSrc)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	add(m, mDst);
	add(m, mSrc);
	add(m, a.srcLayout, b.srcLayout);
	add(m, a.dstLayout, b.dstLayout);
	addSpanOrderedStrict(m, mt, a.blits, b.blits);

	return m;
}

MatchVal match(MatchType mt, const ResolveImageCmd& a, const ResolveImageCmd& b) {
	auto mDst = match(mt, a.dst, b.dst);
	auto mSrc = match(mt, a.src, b.src);
	if(noMatch(mDst) || noMatch(mSrc)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	add(m, mDst);
	add(m, mSrc);
	add(m, a.srcLayout, b.srcLayout);
	add(m, a.dstLayout, b.dstLayout);
	addSpanOrderedStrict(m, mt, a.regions, b.regions);

	return m;
}

MatchVal match(MatchType mt, const CopyBufferCmd& a, const CopyBufferCmd& b) {
	auto mDst = match(mt, a.dst, b.dst);
	auto mSrc = match(mt, a.src, b.src);
	if(noMatch(mDst) || noMatch(mSrc)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	add(m, mDst);
	add(m, mSrc);

	addSpanOrderedStrict(m, mt, a.regions, b.regions);

	return m;
}

MatchVal match(MatchType mt, const UpdateBufferCmd& a, const UpdateBufferCmd& b) {
	auto m = match(mt, a.dst, b.dst);
	if(noMatch(m)) {
		return m;
	}

	// we intentionally don't compare the data contents here
	add(m, a.data.size(), a.data.size());
	add(m, a.offset, b.offset, 0.2);
	return m;
}

MatchVal match(MatchType mt, const FillBufferCmd& a, const FillBufferCmd& b) {
	auto m = match(mt, a.dst, b.dst);
	if(noMatch(m)) {
		return m;
	}

	add(m, a.data, b.data);
	add(m, a.size, b.size);
	add(m, a.offset, b.offset, 0.2);
	return m;
}

MatchVal match(MatchType mt, const ClearColorImageCmd& a, const ClearColorImageCmd& b) {
	auto m = match(mt, a.dst, b.dst);
	if(noMatch(m)) {
		return m;
	}

	add(m, a.dstLayout, b.dstLayout, 2.0);
	addSpanOrderedStrict(m, mt, a.ranges, b.ranges);

	// TODO union comparison is sketchy
	m.total += 1;
	if(std::memcmp(&a.color.uint32, &b.color.uint32, sizeof(a.color)) == 0u) {
		m.match += 1;
	}

	return m;
}

MatchVal match(MatchType mt, const ClearDepthStencilImageCmd& a, const ClearDepthStencilImageCmd& b) {
	auto m = match(mt, a.dst, b.dst);
	if(noMatch(m)) {
		return m;
	}

	add(m, a.dstLayout, b.dstLayout, 2.0);
	addSpanOrderedStrict(m, mt, a.ranges, b.ranges);

	m.total += 1;
	if(std::memcmp(&a.value, &b.value, sizeof(a.value)) == 0u) {
		m.match += 1;
	}

	return m;
}

MatchVal match(MatchType mt, const ClearAttachmentCmd& a, const ClearAttachmentCmd& b) {
	MatchVal m;
	addSpanOrderedStrict(m, mt, a.attachments, b.attachments, 5.0);
	addSpanOrderedStrict(m, mt, a.rects, b.rects);
	return m;
}

MatchVal match(MatchType, const BeginDebugUtilsLabelCmd& a, const BeginDebugUtilsLabelCmd& b) {
	if(std::strcmp(a.name, b.name) != 0) {
		return MatchVal::noMatch();
	}

	return MatchVal{4.f, 4.f};
}

MatchVal match(MatchType, const PushConstantsCmd& a, const PushConstantsCmd& b) {
	dlg_assert_or(a.pipeLayout, return MatchVal::noMatch());
	dlg_assert_or(b.pipeLayout, return MatchVal::noMatch());

	// hard matching on metadata here. The data is irrelevant when
	// the push destination isn't the same.
	if(conflicting(*a.pipeLayout, *b.pipeLayout) ||
			a.stages != b.stages ||
			a.offset != b.offset ||
			a.values.size() != b.values.size()) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	m.total += 4.f;
	m.match += 4.f;

	if(std::memcmp(a.values.data(), b.values.data(), a.values.size()) == 0u) {
		m.total += 1.f;
		m.match += 1.f;
	}

	return m;
}

MatchVal match(MatchType mt, const TraceRaysCmd& a, const TraceRaysCmd& b) {
	auto m = doMatch(mt, a, b);
	if(noMatch(m)) {
		return MatchVal::noMatch();
	}

	// we don't hard-match on them since this may change for per-frame
	// varying workloads (in comparison to draw parameters, which rarely
	// change for per-frame stuff). The higher the dimension, the more unlikely
	// this gets though.
	add(m, a.width, b.width, 2.0);
	add(m, a.height, b.height, 4.0);
	add(m, a.depth, b.depth, 6.0);

	add(m, a.raygenBindingTable.stride, b.raygenBindingTable.stride);
	add(m, a.hitBindingTable.stride, b.hitBindingTable.stride);
	add(m, a.missBindingTable.stride, b.missBindingTable.stride);
	add(m, a.callableBindingTable.stride, b.callableBindingTable.stride);

	return m;
}

MatchVal match(MatchType mt, const TraceRaysIndirectCmd& a, const TraceRaysIndirectCmd& b) {
	auto m = doMatch(mt, a, b);
	if(m.total == -1.f) {
		return MatchVal::noMatch();
	}

	add(m, a.raygenBindingTable.stride, b.raygenBindingTable.stride);
	add(m, a.hitBindingTable.stride, b.hitBindingTable.stride);
	add(m, a.missBindingTable.stride, b.missBindingTable.stride);
	add(m, a.callableBindingTable.stride, b.callableBindingTable.stride);

	return m;
}

MatchVal match(MatchType mt, const BeginRenderingCmd& a, const BeginRenderingCmd& b) {
	MatchVal m;

	// NOTE: we are very conservative here, might want to relax it in the future

	// Match attachments. We are strict here, we don't have any other information
	if(a.colorAttachments.size() != b.colorAttachments.size()) {
		return MatchVal::noMatch();
	}

	using Attachment = BeginRenderingCmd::Attachment;
	auto addAttachment = [&](const Attachment& a, const Attachment& b) {
		if(!a.view && !b.view) {
			m.match += 1.f;
			m.total += 1.f;
			return true;
		}

		if((a.resolveView || b.resolveView)) {
			if(a.resolveMode != b.resolveMode || a.resolveImageLayout != b.resolveImageLayout) {
				return false;
			}

			auto rvMatch = match(mt, a.resolveView, b.resolveView);
			if(noMatch(rvMatch)) {
				return false;
			}

			add(m, rvMatch);
		}

		// NOTE: clear value irrelevant, might change from frame to frame
		auto vMatch = match(mt, a.view, b.view);
		if(noMatch(vMatch)) {
			return false;
		}

		return a.loadOp == b.loadOp &&
			a.storeOp == b.storeOp &&
			a.imageLayout == b.imageLayout;
	};

	for(auto i = 0u; i < a.colorAttachments.size(); ++i) {
		if(!addAttachment(a.colorAttachments[i], b.colorAttachments[i])) {
			return MatchVal::noMatch();
		}
	}

	if(!addAttachment(a.depthAttachment, b.depthAttachment) ||
			!addAttachment(a.stencilAttachment, b.stencilAttachment)) {
		return MatchVal::noMatch();
	}

	if(a.flags != b.flags ||
			a.viewMask != b.viewMask ||
			a.layerCount != b.layerCount) {
		return MatchVal::noMatch();
	}

	m.match += 4.f;
	m.total += 4.f;
	return m;
}

MatchVal matchAccelStructBuild(
		MatchType mt,
		// a
		const VkAccelerationStructureBuildGeometryInfoKHR& geomsA,
		span<const VkAccelerationStructureBuildRangeInfoKHR> rangesA,
		const AccelStruct* srcA,
		const AccelStruct* dstA,
		// b
		const VkAccelerationStructureBuildGeometryInfoKHR& geomsB,
		span<const VkAccelerationStructureBuildRangeInfoKHR> rangesB,
		const AccelStruct* srcB,
		const AccelStruct* dstB) {
	if(geomsA.type != geomsB.type || geomsA.mode != geomsB.mode) {
		return MatchVal::noMatch();
	}

	// TODO: consider other parameters
	(void) rangesA;
	(void) srcA;
	(void) rangesB;
	(void) srcB;

	return match(mt, dstA, dstB);
}

MatchVal match(MatchType mt, const BuildAccelStructsCmd& a, const BuildAccelStructsCmd& b) {
	auto cbBuild = [&](auto ia, auto ib) {
		return matchAccelStructBuild(mt,
			a.buildInfos[ia], a.buildRangeInfos[ia], a.srcs[ia], a.dsts[ia],
			b.buildInfos[ib], b.buildRangeInfos[ib], b.srcs[ib], b.dsts[ib]);
	};

	MatchVal ret;
	addSpanOrderedStrict(ret, a.buildInfos.size(), b.buildInfos.size(), cbBuild);
	return ret;
}

// dummy for validExpression below
template<typename Cmd> using HasStaticType = decltype(Cmd::staticType());
template<typename Cmd> using HasMatch = decltype(match(MatchType::deep,
		std::declval<Cmd>(), std::declval<Cmd>()));

template<typename Cmd>
MatchVal invokeCommandMatch(MatchType mt, const Cmd& a, const Command& base) {
	static_assert(!std::is_reference_v<Cmd>);
	static_assert(!std::is_pointer_v<Cmd>);
	static_assert(!std::is_const_v<Cmd>);

	if constexpr(validExpression<HasStaticType, Cmd>) {
		auto* b = commandCast<const Cmd*>(&base);
		if(!b) {
			return MatchVal::noMatch();
		}

		if constexpr(validExpression<HasMatch, Cmd>) {
			return match(mt, a, *b);
		} else {
			dlg_trace("Matching for command {} not implemented",
				typeid(a).name());

			return MatchVal{1.f, 1.f};
		}
	} else {
		dlg_warn("Command visitor for {} not implemented (Cmd = {})",
			typeid(a).name(), typeid(Cmd).name());
		return MatchVal::noMatch();
	}
}

// Returns how much this commands matches with the given one.
// Will always return 0.f for two commands that don't have the
// same type. Should not consider child commands, just itself.
// Will also never consider the stackTrace.
MatchVal match(const Command& a, const Command& b, MatchType matchType) {
	MatchVal ret;
	auto invoker = [&](const auto& cmd) {
		ret = invokeCommandMatch(matchType, cmd, b);
	};
	auto visitor = TemplateCommandVisitor(std::move(invoker));
	a.visit(visitor);

	return ret;
}

} // namespace vil
