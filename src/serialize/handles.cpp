#include <serialize/serialize.hpp>
#include <serialize/util.hpp>
#include <serialize/internal.hpp>

#include <image.hpp>
#include <buffer.hpp>
#include <memory.hpp>
#include <pipe.hpp>
#include <rp.hpp>
#include <ds.hpp>
#include <shader.hpp>
#include <accelStruct.hpp>
#include <queue.hpp>
#include <swapchain.hpp>
#include <sync.hpp>
#include <cb.hpp>
#include <queryPool.hpp>
#include <vkutil/enumString.hpp>

namespace vil {

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, Buffer& buf) {
	serialize(slz, io, static_cast<MemoryResource&>(buf), VK_OBJECT_TYPE_BUFFER);

	serialize(io, buf.ci.size);
	serialize(io, buf.ci.usage);
	serialize(io, buf.ci.flags);
	serialize(io, buf.ci.sharingMode);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, BufferView& bv) {
	serialize(io, bv.name);
	serializeRef(slz, io, bv.buffer);

	if(std::is_same_v<Slz, StateLoader> && bv.buffer) {
		bv.buffer->views.push_back(&bv);
	}

	serialize(io, bv.ci.flags);
	serialize(io, bv.ci.format);
	serialize(io, bv.ci.offset);
	serialize(io, bv.ci.range);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io,
		DescriptorSetLayout& layout) {
	serialize(io, layout.name);
	serialize(io, layout.flags);

	auto serializeBinding = [&](auto& buf, DescriptorSetLayout::Binding& binding) {
		serialize(buf, binding.descriptorCount);
		if(binding.descriptorCount == 0u) {
			return;
		}

		serialize(buf, binding.descriptorType);
		serialize(buf, binding.dynOffset);
		serialize(buf, binding.flags);
		serialize(buf, binding.stageFlags);
		serialize(buf, binding.offset);

		bool hasImmutableSamplers;
		if constexpr(std::is_same_v<Slz, StateLoader>) {
			hasImmutableSamplers = read<u8>(buf);
			if(hasImmutableSamplers) {
				binding.immutableSamplers = std::make_unique<
					IntrusivePtr<Sampler>[]>(binding.descriptorCount);
			}
		} else {
			hasImmutableSamplers = !!binding.immutableSamplers;
			write<u8>(buf, hasImmutableSamplers);
		}

		if(hasImmutableSamplers) {
			for(auto i = 0u; i < binding.descriptorCount; ++i) {
				serializeRef(slz, io, binding.immutableSamplers[i]);
			}
		}
	};

	serializeContainer(io, layout.bindings, serializeBinding);
}

template<typename Slz, typename IO> void serializeStage(Slz& slz, IO& io,
		PipelineShaderStage& stage) {
	serialize(io, stage.stage);
	serialize(io, stage.entryPoint);
	serializeContainer(io, stage.specialization.entries);
	// TODO: should provide fast memcpy overload for this
	serializeContainer(io, stage.specialization.data);
	serializeRef(slz, io, stage.spirv);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, PipelineLayout& layout) {
	serialize(io, layout.name);
	serializeContainer(io, layout.pushConstants);
	serializeRefs(slz, io, layout.descriptors);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, GraphicsPipeline& pipe) {
	serialize(io, pipe.name);
	serializeRef(slz, io, pipe.layout);
	serializeRef(slz, io, pipe.renderPass);
	serialize(io, pipe.subpass);

	serializeContainer(io, pipe.stages, [&](auto& buf, auto& stage) {
		serializeStage(slz, buf, stage);
	});

	serializeContainer(io, pipe.dynamicState);

	serializeContainer(io, pipe.viewports);
	serializeContainer(io, pipe.scissors);

	serializeContainer(io, pipe.vertexAttribs);
	serializeContainer(io, pipe.vertexBindings);

	serializeContainer(io, pipe.blendAttachments);

	serialize(io, pipe.depthStencilState.depthBoundsTestEnable);
	serialize(io, pipe.depthStencilState.depthWriteEnable);
	serialize(io, pipe.depthStencilState.depthCompareOp);
	serialize(io, pipe.depthStencilState.depthBoundsTestEnable);
	serialize(io, pipe.depthStencilState.stencilTestEnable);
	serialize(io, pipe.depthStencilState.front);
	serialize(io, pipe.depthStencilState.back);
	serialize(io, pipe.depthStencilState.minDepthBounds);
	serialize(io, pipe.depthStencilState.maxDepthBounds);

	serialize(io, pipe.inputAssemblyState.primitiveRestartEnable);
	serialize(io, pipe.inputAssemblyState.topology);

	serialize(io, pipe.rasterizationState.cullMode);
	serialize(io, pipe.rasterizationState.rasterizerDiscardEnable);
	serialize(io, pipe.rasterizationState.lineWidth);
	serialize(io, pipe.rasterizationState.polygonMode);
	serialize(io, pipe.rasterizationState.frontFace);
	serialize(io, pipe.rasterizationState.depthBiasEnable);
	serialize(io, pipe.rasterizationState.depthBiasClamp);
	serialize(io, pipe.rasterizationState.depthBiasConstantFactor);
	serialize(io, pipe.rasterizationState.depthBiasSlopeFactor);

	serialize(io, pipe.multisampleState.rasterizationSamples);
	serialize(io, pipe.multisampleState.alphaToCoverageEnable);
	serialize(io, pipe.multisampleState.alphaToOneEnable);
	serialize(io, pipe.multisampleState.sampleShadingEnable);
	serialize(io, pipe.multisampleState.flags);
	serialize(io, pipe.multisampleState.minSampleShading);

	if constexpr(std::is_same_v<Slz, StateLoader>) {
		fixPointers(pipe);
	}

	// TODO: other graphics states
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, RayTracingPipeline& pipe) {
	serialize(io, pipe.name);
	serializeRef(slz, io, pipe.layout);
	serializeContainer(io, pipe.stages, [&](auto& buf, auto& stage) {
		serializeStage(slz, buf, stage);
	});

	serializeContainer(io, pipe.groups);
	serializeContainer(io, pipe.dynamicState);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, ComputePipeline& pipe) {
	serialize(io, pipe.name);
	serializeRef(slz, io, pipe.layout);
	serializeStage(slz, io, pipe.stage);
}

template<typename Slz, typename IO> void serialize(Slz&, IO& io, ShaderModule& mod) {
	serialize(io, mod.name);
	serialize(io, mod.spirvHash);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io, MemoryBind& bind) {
	serialize(io, bind.memOffset);
	serialize(io, bind.memSize);
	serializeRef(slz, io, bind.resource);
}

template<typename Slz, typename IO> void serialize(Slz&, IO& io, DeviceMemory& mem) {
	serialize(io, mem.name);
	// TODO: not portable. Could instead save the memory flags and try
	//   to find a matching mem type on load?
	serialize(io, mem.typeIndex);
	serialize(io, mem.size);

	// NOTE: we don't serialize allocations, the serialized resources will
	// add themselves, see serialization of MemoryResource below
}

void finishLoad(MemoryBind& bind, MemoryResource& res) {
	bind.resource = &res;
	if(bind.memory) {
		bind.memory->allocations.insert(&bind);
	}
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io,
		FullMemoryBind& bind, MemoryResource& res) {
	serialize(io, bind.memOffset);
	serialize(io, bind.memSize);
	serialize(io, bind.memState); // TODO: could be computed on load I guess
	serializeRef(slz, io, bind.memory);

	if constexpr(std::is_same_v<Slz, StateLoader>) {
		finishLoad(bind, res);
	}
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, SparseMemoryState& bind, MemoryResource& res) {
	auto opaqueBindSerialize = [&](auto& buf, OpaqueSparseMemoryBind& bind) {
		serialize(buf, bind.memOffset);
		serialize(buf, bind.memSize);
		serialize(buf, bind.flags);
		serialize(buf, bind.resourceOffset);
		serializeRef(slz, buf, bind.memory);

		if constexpr(std::is_same_v<Slz, StateLoader>) {
			finishLoad(bind, res);
		}
	};

	auto imgBindSerialize = [&](auto& buf, ImageSparseMemoryBind& bind) {
		serialize(buf, bind.memOffset);
		serialize(buf, bind.memSize);
		serialize(buf, bind.flags);
		serialize(buf, bind.subres);
		serialize(buf, bind.offset);
		serialize(buf, bind.size);
		serializeRef(slz, buf, bind.memory);

		if constexpr(std::is_same_v<Slz, StateLoader>) {
			finishLoad(bind, res);
		}
	};

	serializeContainer(io, bind.opaqueBinds, opaqueBindSerialize);
	serializeContainer(io, bind.imageBinds, imgBindSerialize);
}

template<typename Slz, typename IO> void serialize(Slz& slz, IO& io,
		MemoryResource& res, VkObjectType type) {
	serialize(io, res.name);

	if constexpr(std::is_same_v<Slz, StateSaver>) {
		dlg_assert(res.memObjectType == type);
		write<u8>(io, res.memory.index());
		std::visit([&](auto& mem) { serialize(slz, io, mem, res); }, res.memory);
	} else {
		res.memObjectType = type;
		auto index = read<u8>(io);
		if(index == 0u) {
			auto& mem = res.memory.emplace<0>();
			serialize(slz, io, mem, res);
		} else if(index == 1u) {
			auto& mem = res.memory.emplace<1>();
			serialize(slz, io, mem, res);
		} else {
			throw std::runtime_error("Invalid memory type");
		}
	}
}


template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, Image& img) {
	serialize(slz, io, static_cast<MemoryResource&>(img), VK_OBJECT_TYPE_IMAGE);

	serialize(io, img.ci.imageType);
	serialize(io, img.ci.extent);
	serialize(io, img.ci.arrayLayers);
	serialize(io, img.ci.mipLevels);
	serialize(io, img.ci.flags);
	serialize(io, img.ci.format);
	serialize(io, img.ci.initialLayout);
	serialize(io, img.ci.samples);
	serialize(io, img.ci.tiling);
	serialize(io, img.ci.usage);
	serialize(io, img.ci.sharingMode);
	// TODO queue families

	// NOTE: views aren't serialized, they add themself on load
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, ImageView& iv) {
	serialize(io, iv.name);
	serializeRef(slz, io, iv.img);

	if(std::is_same_v<Slz, StateLoader> && iv.img) {
		iv.img->views.push_back(&iv);
	}

	serialize(io, iv.ci.flags);
	serialize(io, iv.ci.viewType);
	serialize(io, iv.ci.format);
	serialize(io, iv.ci.components);
	serialize(io, iv.ci.subresourceRange);
	serialize(io, iv.ci.viewType);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, Sampler& sampler) {
	serialize(io, sampler.name);
	serialize(io, sampler.ci.flags);
	serialize(io, sampler.ci.magFilter);
	serialize(io, sampler.ci.minFilter);
	serialize(io, sampler.ci.mipmapMode);
	serialize(io, sampler.ci.addressModeU);
	serialize(io, sampler.ci.addressModeV);
	serialize(io, sampler.ci.addressModeW);
	serialize(io, sampler.ci.mipLodBias);
	serialize(io, sampler.ci.anisotropyEnable);
	serialize(io, sampler.ci.maxAnisotropy);
	serialize(io, sampler.ci.compareEnable);
	serialize(io, sampler.ci.compareOp);
	serialize(io, sampler.ci.minLod);
	serialize(io, sampler.ci.maxLod);
	serialize(io, sampler.ci.borderColor);
	serialize(io, sampler.ci.unnormalizedCoordinates);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, Framebuffer& fb) {
	serialize(io, fb.name);

	serialize(io, fb.width);
	serialize(io, fb.height);
	serialize(io, fb.layers);
	serialize(io, fb.imageless);
	serializeRef(slz, io, fb.rp);

	auto attachmentSerializer = [&](auto& buf, ImageView*& view) {
		serializeRef(slz, buf, view);
		if(view && std::is_same_v<Slz, StateLoader>) {
			view->fbs.push_back(&fb);
		}
	};

	serializeContainer(io, fb.attachments, attachmentSerializer);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, RenderPass& rp) {
	serialize(io, rp.name);

	auto serializeAttRef = [&](auto& buf, VkAttachmentReference2& ref) {
		serialize(buf, ref.attachment);
		serialize(buf, ref.aspectMask);
		serialize(buf, ref.layout);
	};

	auto serializeAttRefs = [&](auto& buf, const VkAttachmentReference2*& refs, u32& count) {
		if constexpr(std::is_same_v<Slz, StateLoader>) {
			std::vector<VkAttachmentReference2> tmp;
			readContainer(buf, tmp, serializeAttRef);
			count = tmp.size();

			if(tmp.empty()) {
				refs = nullptr;
				return;
			}

			refs = tmp.data();
			rp.desc.attachmentRefs.push_back(tmp);
		} else {
			writeContainer(buf, span{refs, count}, serializeAttRef);
		}
	};

	auto serializeAttIDs = [&](auto& buf, const u32*& refs, u32& count) {
		if constexpr(std::is_same_v<Slz, StateLoader>) {
			std::vector<u32> tmp;
			readContainer(buf, tmp);
			count = tmp.size();

			if(tmp.empty()) {
				refs = nullptr;
				return;
			}

			refs = tmp.data();
			rp.desc.attachmentIDs.push_back(tmp);
		} else {
			writeContainer(buf, span{refs, count});
		}
	};

	auto serializeSubpass = [&](auto& buf, VkSubpassDescription2& sp) {
		serialize(buf, sp.flags);
		serialize(buf, sp.viewMask);

		assertLoadImmediate(slz, sp.pipelineBindPoint, VK_PIPELINE_BIND_POINT_GRAPHICS);

		u32 depthStencilCount;
		u32 resolveCount;

		if constexpr(std::is_same_v<Slz, StateSaver>) {
			depthStencilCount = sp.pDepthStencilAttachment ? 1u : 0u;
			resolveCount = sp.pResolveAttachments ? sp.colorAttachmentCount : 0u;
		}

		serializeAttRefs(buf, sp.pColorAttachments, sp.colorAttachmentCount);
		serializeAttRefs(buf, sp.pDepthStencilAttachment, depthStencilCount);
		serializeAttRefs(buf, sp.pResolveAttachments, resolveCount);
		serializeAttIDs(buf, sp.pPreserveAttachments, sp.preserveAttachmentCount);
	};

	auto serializeDep = [&](auto& buf, VkSubpassDependency2& dep) {
		serialize(buf, dep.srcSubpass);
		serialize(buf, dep.dstSubpass);
		serialize(buf, dep.srcAccessMask);
		serialize(buf, dep.dstAccessMask);
		serialize(buf, dep.srcStageMask);
		serialize(buf, dep.dstStageMask);
		serialize(buf, dep.dependencyFlags);
		serialize(buf, dep.viewOffset);
	};

	auto serializeAttachment = [&](auto& buf, VkAttachmentDescription2& att) {
		serialize(buf, att.flags);
		serialize(buf, att.format);
		serialize(buf, att.samples);
		serialize(buf, att.loadOp);
		serialize(buf, att.storeOp);
		serialize(buf, att.stencilLoadOp);
		serialize(buf, att.stencilStoreOp);
		serialize(buf, att.initialLayout);
		serialize(buf, att.finalLayout);
	};

	serializeContainer(io, rp.desc.subpasses, serializeSubpass);
	serializeContainer(io, rp.desc.dependencies, serializeDep);
	serializeContainer(io, rp.desc.attachments, serializeAttachment);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, Event& event) {
	serialize(io, event.name);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, Fence& fence) {
	serialize(io, fence.name);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, Semaphore& semaphore) {
	serialize(io, semaphore.name);
	serialize(io, semaphore.type);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, AccelStruct& accelStruct) {
	serialize(io, accelStruct.name);
	serialize(io, accelStruct.type);
	serialize(io, accelStruct.effectiveType);
	serialize(io, accelStruct.type);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, DescriptorPool& pool) {
	serialize(io, pool.name);
	serializeContainer(io, pool.poolSizes);
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, DescriptorUpdateTemplate& dut) {
	serialize(io, dut.name);
	serializeContainer(io, dut.entries);
}

template<typename Slz, typename IO, typename H> using HasSerialize =
	decltype(serialize(std::declval<Slz>(), std::declval<IO>(), std::declval<H>()));

void writeHandle(StateSaver& saver, u32 handleID,
		const Handle& handle, VkObjectType type) {
	auto typeHandler = ObjectTypeHandler::handler(type);

	serializeMarker(saver.handleBuf, markerStartHandle + handleID,
		dlg::format("handle {}", handleID));

	static_assert(validExpression<HasSerialize,
		StateSaver&,
		std::add_lvalue_reference_t<decltype(StateSaver::handleBuf)>,
		Buffer&>, "Detection typedef broken");

	auto serializeVisitor = TemplateResourceVisitor([&](auto& res) {
		using HT = std::remove_reference_t<decltype(res)>;
		constexpr auto valid = validExpression<HasSerialize,
			StateSaver&,
			std::add_lvalue_reference_t<decltype(saver.handleBuf)>,
			HT&>;
		if constexpr(valid) {
			serialize(saver, saver.handleBuf, res);
		} else {
			dlg_error("Error: tried to serialize {}", vk::name(res.objectType));
			throw std::runtime_error("Invalid handle type");
		}
	});

	typeHandler->visit(serializeVisitor, const_cast<Handle&>(handle));
}

template<typename T>
auto& addUnique(StateLoader& loader) {
	auto ptr = new T();
	auto dtor = [](Handle& handle) {
		delete &static_cast<T&>(handle);
	};
	loader.destructors.push_back(dtor);
	loader.handles.push_back(ptr);
	return *ptr;
}

template<typename T>
auto& addIntrusive(StateLoader& loader) {
	auto ptr = new T();
	++ptr->refCount;
	auto dtor = [](Handle& handle) {
		RefCountHandler<T>{}.dec(static_cast<T&>(handle));
	};
	loader.destructors.push_back(dtor);
	loader.handles.push_back(ptr);
	return *ptr;
}

Pipeline& addPipe(StateLoader& loader, VkPipelineBindPoint pbp) {
	switch(pbp) {
		case VK_PIPELINE_BIND_POINT_GRAPHICS: return addIntrusive<GraphicsPipeline>(loader);
		case VK_PIPELINE_BIND_POINT_COMPUTE: return addIntrusive<ComputePipeline>(loader);
		case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: return addIntrusive<RayTracingPipeline>(loader);
		default:
			dlg_error("Unexpected pipeline bind point {}", pbp);
			throw std::runtime_error("Invalid pipeline type");
	}
}

Handle& addHandle(StateLoader& loader) {
	auto type = read<VkObjectType>(loader.buf);
	loader.handleTypes.push_back(type);

	// TODO: does it even matter if we create stuff via delete dotr or
	// decRefCount dtor?
	// seems kinda stupid to have handles destroyed when the StateLoader
	// is destroyed. Proper way would be something like "struct HandleSet"
	// that can be extracted from the loader.
	// Hm, but that again would then need exactly this dtor list.
	// fair enough.
	switch(type) {
		case VK_OBJECT_TYPE_IMAGE: return addIntrusive<Image>(loader);
		case VK_OBJECT_TYPE_BUFFER: return addIntrusive<Buffer>(loader);
		case VK_OBJECT_TYPE_IMAGE_VIEW: return addIntrusive<ImageView>(loader);
		case VK_OBJECT_TYPE_BUFFER_VIEW: return addIntrusive<BufferView>(loader);
		case VK_OBJECT_TYPE_SAMPLER: return addIntrusive<Sampler>(loader);
		case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return addIntrusive<DescriptorSetLayout>(loader);
		case VK_OBJECT_TYPE_RENDER_PASS: return addIntrusive<RenderPass>(loader);
		case VK_OBJECT_TYPE_FRAMEBUFFER: return addIntrusive<Framebuffer>(loader);
		case VK_OBJECT_TYPE_DEVICE_MEMORY: return addIntrusive<DeviceMemory>(loader);
		case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return addIntrusive<PipelineLayout>(loader);
		case VK_OBJECT_TYPE_SHADER_MODULE: return addIntrusive<ShaderModule>(loader);
		case VK_OBJECT_TYPE_EVENT: return addIntrusive<Event>(loader);
		case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return addIntrusive<AccelStruct>(loader);
		case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return addIntrusive<DescriptorPool>(loader);
		case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return addIntrusive<DescriptorUpdateTemplate>(loader);
		case VK_OBJECT_TYPE_PIPELINE: {
			auto pbp = read<VkPipelineBindPoint>(loader.buf);
			auto& pipe = addPipe(loader, pbp);
			pipe.type = pbp;
			return pipe;
		} default:
			dlg_error("Unexpected handle type {}", type);
			throw std::runtime_error("Invalid handle type");
	}
}

void readHandles(StateLoader& loader) {
	dlg_assert(loader.handles.size() == loader.handleTypes.size());
	for(auto i = 0u; i < loader.handles.size(); ++i) {
		auto type = loader.handleTypes[i];
		auto typeHandler = ObjectTypeHandler::handler(type);

		serializeMarker(loader.buf, markerStartHandle + i,
			dlg::format("handle {}", i));

		static_assert(validExpression<HasSerialize,
			StateLoader&,
			std::add_lvalue_reference_t<decltype(loader.buf)>,
			Buffer&>, "Detection typedef broken");

		auto serializeVisitor = TemplateResourceVisitor([&](auto& res) {
			using HT = std::remove_reference_t<decltype(res)>;
			constexpr auto valid = validExpression<HasSerialize,
				StateLoader&,
				std::add_lvalue_reference_t<decltype(loader.buf)>,
				HT&>;
			if constexpr(valid) {
				serialize(loader, loader.buf, res);
			} else {
				dlg_error("Error: tried to serialize {}", vk::name(res.objectType));
				throw std::runtime_error("Invalid handle type");
			}
		});

		typeHandler->visit(serializeVisitor, *loader.handles[i]);
	}
}

} // namespace
