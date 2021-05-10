#include <ds.hpp>
#include <device.hpp>
#include <data.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>

namespace vil {

// util
size_t descriptorSize(VkDescriptorType dsType) {
	switch(category(dsType)) {
		case DescriptorCategory::buffer: return sizeof(BufferDescriptor);
		case DescriptorCategory::image: return sizeof(ImageDescriptor);
		case DescriptorCategory::bufferView: return sizeof(BufferViewDescriptor);
		case DescriptorCategory::none:
			dlg_error("unreachable: Invalid descriptor category");
			return 0u;
	}

	dlg_error("unreachable");
	return 0u;
}

size_t totalNumBindings(const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	if(layout.bindings.empty()) {
		return 0;
	}

	auto& last = layout.bindings.back();
	size_t ret = last.offset;

	if(last.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		ret += variableDescriptorCount;
	} else {
		ret += last.descriptorCount;
	}

	return ret;
}

size_t totalDescriptorMemSize(const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	if(layout.bindings.empty()) {
		return 0;
	}

	auto& last = layout.bindings.back();
	size_t ret = last.offset;
	auto lastCount = last.descriptorCount;

	if(last.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		ret += variableDescriptorCount;
	}

	ret += lastCount * descriptorSize(last.descriptorType);
	return ret;
}

bool compatible(const DescriptorSetLayout& da, const DescriptorSetLayout& db) {
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
			if(bool(ba.immutableSamplers) != bool(bb.immutableSamplers)) {
				return false;
			}

			if(ba.immutableSamplers) {
				dlg_assert(ba.descriptorCount == bb.descriptorCount);
				for(auto e = 0u; e < ba.descriptorCount; ++e) {
					// TODO: consider compatible (instead of just same)
					// samplers as well?
					if(ba.immutableSamplers[e] != bb.immutableSamplers[e]) {
						return false;
					}
				}
			}
		}
	}

	return true;
}

DescriptorSetStatePtr newDescriptorSetState(
		IntrusivePtr<DescriptorSetLayout> layout, u32 variableDescriptorCount) {
	auto memSize = sizeof(DescriptorSetState) +
		totalDescriptorMemSize(*layout, variableDescriptorCount);
	auto mem = new std::byte[memSize]();
	TracyAllocS(mem, memSize, 8);

	auto* state = new(mem) DescriptorSetState();
	dlg_assert(reinterpret_cast<std::byte*>(state) == mem);

	state->layout = std::move(layout);
	state->variableDescriptorCount = variableDescriptorCount;

	++state->layout->dev->stats.aliveDescriptorStates;

	// initialize descriptors
	auto it = mem + sizeof(DescriptorSetState);
	for(auto& binding : state->layout->bindings) {
		auto count = binding.descriptorCount;
		if(binding.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT) {
			count = variableDescriptorCount;
		}

		switch(category(binding.descriptorType)) {
			case DescriptorCategory::buffer:
				new(it) BufferDescriptor[count];
				it += count * sizeof(BufferDescriptor);
				break;
			case DescriptorCategory::bufferView:
				new(it) ImageDescriptor[count];
				it += count * sizeof(BufferViewDescriptor);
				break;
			case DescriptorCategory::image:
				new(it) ImageDescriptor[count];
				it += count * sizeof(ImageDescriptor);
				break;
			case DescriptorCategory::none:
				dlg_error("unreachable: invalid descriptor type");
				break;
		}
	}

	return DescriptorSetStatePtr(state);
}

void copy(DescriptorSetState& dst, unsigned dstBindID, unsigned dstElemID,
		DescriptorSetState& src, unsigned srcBindID, unsigned srcElemID) {
	auto& srcLayout = src.layout->bindings[srcBindID];
	auto& dstLayout = dst.layout->bindings[dstBindID];
	dlg_assert(srcLayout.descriptorType == dstLayout.descriptorType);

	switch(category(dstLayout.descriptorType)) {
		case DescriptorCategory::image: {
			ImageDescriptor srcCopy;
			{
				std::lock_guard lock(src.mutex);
				srcCopy = images(src, srcBindID)[srcElemID];
			}

			std::lock_guard lock(dst.mutex);
			auto& dstBind = images(dst, dstBindID)[dstElemID];
			dstBind.imageView = std::move(srcCopy.imageView);
			dstBind.layout = srcCopy.layout;

			if(!dstLayout.immutableSamplers.get()) {
				dstBind.sampler = std::move(srcCopy.sampler);
			}
			break;
		} case DescriptorCategory::buffer: {
			BufferDescriptor srcCopy;
			{
				std::lock_guard lock(src.mutex);
				srcCopy = buffers(src, srcBindID)[srcElemID];
			}

			std::lock_guard lock(dst.mutex);
			buffers(dst, dstBindID)[dstElemID] = srcCopy;
			break;
		} case DescriptorCategory::bufferView: {
			BufferViewDescriptor srcCopy;
			{
				std::lock_guard lock(src.mutex);
				srcCopy = bufferViews(src, srcBindID)[srcElemID];
			}

			std::lock_guard lock(dst.mutex);
			bufferViews(dst, dstBindID)[dstElemID] = srcCopy;
			break;
		} case DescriptorCategory::none:
			dlg_error("unreachable: Invalid descriptor type");
			break;
	}
}

void initImmutableSamplersLocked(DescriptorSetState& state) {
	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		// If the binding holds immutable samplers, fill them in.
		// We do this so we don't have to check for immutable samplers
		// every time we read a binding. Also needed for correct
		// invalidation tracking.
		if(state.layout->bindings[b].immutableSamplers.get()) {
			std::lock_guard lock(state.mutex);

			dlg_assert(needsSampler(state.layout->bindings[b].descriptorType));
			auto binds = images(state, b);

			for(auto e = 0u; e < binds.size(); ++e) {
				auto sampler = state.layout->bindings[b].immutableSamplers[e];
				dlg_assert(sampler);
				dlg_assert(sampler->handle);

				binds[e].sampler = sampler;
			}
		}
	}
}

DescriptorSetStatePtr copyDescriptorSetState(DescriptorSetState& state) {
	ZoneScoped;
	vil_assert_not_owned(state.mutex);

	auto ret = newDescriptorSetState(state.layout, state.variableDescriptorCount);

	initImmutableSamplersLocked(*ret);

	// copy descriptors
	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		for(auto e = 0u; e < descriptorCount(state, b); ++e) {
			copy(*ret, b, e, state, b, e);
		}
	}

	return ret;
}

u32 descriptorCount(const DescriptorSetState& state, unsigned binding) {
	dlg_assert(state.layout);
	dlg_assert(binding < state.layout->bindings.size());
	auto& layout = state.layout->bindings[binding];
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT) {
		return state.variableDescriptorCount;
	}

	return layout.descriptorCount;
}

DescriptorSet::~DescriptorSet() {
	if(!dev) {
		return;
	}

	{
		std::lock_guard lock(dev->mutex);

		dlg_assert(state);
		state->ds = nullptr;

		// Remove from descriptor pool.
		// Pools can't be destroyed before their sets as they implicitly free them.
		dlg_assert(pool);

		auto it = find(pool->descriptorSets, this);
		dlg_assert(it != pool->descriptorSets.end());
		pool->descriptorSets.erase(it);
	}

	// make sure to potentially run state destructor outside critical section
	state.reset();

	dlg_assert(dev->stats.aliveDescriptorSets > 0);
	--dev->stats.aliveDescriptorSets;
}

/*
span<DescriptorBinding> bindings(DescriptorSetState& state, unsigned binding) {
	dlg_assert(state.layout);
	dlg_assert(binding < state.layout->bindings.size());
	auto& layout = state.layout->bindings[binding];
	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT) {
		count = state.variableDescriptorCount;
	}

	auto* base = reinterpret_cast<std::byte*>(&state) + sizeof(DescriptorSetState);
	auto* bindings = std::launder(reinterpret_cast<DescriptorBinding*>(base));
	return {bindings + layout.offset, count};
}

span<const DescriptorBinding> bindings(const DescriptorSetState& state, unsigned binding) {
	dlg_assert(state.layout);
	dlg_assert(binding < state.layout->bindings.size());
	auto& layout = state.layout->bindings[binding];
	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT) {
		count = state.variableDescriptorCount;
	}

	auto* base = reinterpret_cast<const std::byte*>(&state) + sizeof(DescriptorSetState);
	auto* bindings = std::launder(reinterpret_cast<const DescriptorBinding*>(base));
	return {bindings + layout.offset, count};
}

DescriptorBinding& binding(DescriptorSetState& state,
		unsigned binding, unsigned elem) {
	auto bs = bindings(state, binding);
	dlg_assert(elem < bs.size());
	return bs[elem];
}

const DescriptorBinding& binding(const DescriptorSetState& state,
		unsigned binding, unsigned elem) {
	auto bs = bindings(state, binding);
	dlg_assert(elem < bs.size());
	return bs[elem];
}
*/

span<BufferDescriptor> buffers(DescriptorSetState& state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::buffer);

	auto ptr = reinterpret_cast<std::byte*>(&state);
	ptr += sizeof(state);
	ptr += layout.offset;

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto d = std::launder(reinterpret_cast<BufferDescriptor*>(ptr));
	return {d, count};
}
span<const BufferDescriptor> buffers(const DescriptorSetState& state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::buffer);

	auto ptr = reinterpret_cast<const std::byte*>(&state);
	ptr += sizeof(state);
	ptr += layout.offset;

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto d = std::launder(reinterpret_cast<const BufferDescriptor*>(ptr));
	return {d, count};
}
span<ImageDescriptor> images(DescriptorSetState& state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::image);

	auto ptr = reinterpret_cast<std::byte*>(&state);
	ptr += sizeof(state);
	ptr += layout.offset;

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto d = std::launder(reinterpret_cast<ImageDescriptor*>(ptr));
	return {d, count};
}
span<const ImageDescriptor> images(const DescriptorSetState& state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::image);

	auto ptr = reinterpret_cast<const std::byte*>(&state);
	ptr += sizeof(state);
	ptr += layout.offset;

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto d = std::launder(reinterpret_cast<const ImageDescriptor*>(ptr));
	return {d, count};
}
span<BufferViewDescriptor> bufferViews(DescriptorSetState& state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::image);

	auto ptr = reinterpret_cast<std::byte*>(&state);
	ptr += sizeof(state);
	ptr += layout.offset;

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto d = std::launder(reinterpret_cast<BufferViewDescriptor*>(ptr));
	return {d, count};
}
span<const BufferViewDescriptor> bufferViews(const DescriptorSetState& state, unsigned binding) {
	dlg_assert(binding < state.layout->bindings.size());

	auto& layout = state.layout->bindings[binding];
	dlg_assert(category(layout.descriptorType) == DescriptorCategory::image);

	auto ptr = reinterpret_cast<const std::byte*>(&state);
	ptr += sizeof(state);
	ptr += layout.offset;

	auto count = layout.descriptorCount;
	if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		count = state.variableDescriptorCount;
	}

	auto d = std::launder(reinterpret_cast<const BufferViewDescriptor*>(ptr));
	return {d, count};
}

DescriptorPool::~DescriptorPool() {
	if(!dev) {
		return;
	}

	// NOTE: we don't need a lock here:
	// While the ds pool is being destroyed, no descriptor sets from it
	// can be created or destroyed in another thread, that would always be a
	// race. So accessing this vector is safe.
	// (Just adding a lock here would furthermore result in deadlocks due
	// to the mutexes locked inside the loop, don't do it!)
	// We don't use a for loop since the descriptors remove themselves
	// on destruction
	while(!descriptorSets.empty()) {
		if(HandleDesc<VkDescriptorSet>::wrap) {
			// TODO: ugh, this is terrible, not sure how to handle the case properly
			auto h = u64ToHandle<VkDescriptorSet>(reinterpret_cast<std::uintptr_t>(descriptorSets[0]));
			dev->descriptorSets.mustErase(h);
		} else {
			auto* ds = descriptorSets[0];
			dev->descriptorSets.mustErase(ds->handle);
		}
	}
}

DescriptorSetLayout::~DescriptorSetLayout() {
	if(!dev) {
		return;
	}

	// ds layouts are never used directly by command buffers
	dlg_assert(refRecords.empty());
	dlg_assert(handle);

	dev->dispatch.DestroyDescriptorSetLayout(dev->handle, handle, nullptr);
}

DescriptorUpdateTemplate::~DescriptorUpdateTemplate() {
	if(!dev) {
		return;
	}

	// never used directly by command buffers
	dlg_assert(refRecords.empty());
	dlg_assert(handle);

	dev->dispatch.DestroyDescriptorUpdateTemplate(dev->handle, handle, nullptr);
}

// util
DescriptorCategory category(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return DescriptorCategory::image;
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			return DescriptorCategory::buffer;
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			return DescriptorCategory::bufferView;
		default:
			dlg_trace("Unknown descriptor type: {}", type);
			return DescriptorCategory::none;
	}
}

bool needsSampler(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			return true;
		default:
			return false;
	}
}

bool needsBoundSampler(const DescriptorSetLayout& dsl, unsigned binding) {
	auto& bind = dsl.bindings[binding];
	return needsSampler(bind.descriptorType) && !bind.immutableSamplers;
}

bool needsImageView(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return true;
		default:
			return false;
	}
}

bool needsImageLayout(VkDescriptorType type) {
	switch(type) {
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			return true;
		default:
			return false;
	}
}

// dsLayout
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(
		VkDevice                                    device,
		const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorSetLayout*                      pSetLayout) {
	// NOTE: we don't use host allocators here since this handle is potentially
	// kept alive inside the layer, preventing us from passing an application
	// allocator to the destruction function
	// See design.md on allocators.
	(void) pAllocator;

	auto& dev = getDevice(device);

	// unwrap immutable sampler handles
	auto nci = *pCreateInfo;
	auto nbindings = LocalVector(nci.pBindings, nci.bindingCount);
	nci.pBindings = nbindings.data();

	ThreadMemScope memScope;
	for(auto& bind : nbindings) {
		if(!needsSampler(bind.descriptorType) || bind.descriptorCount == 0 ||
				!bind.pImmutableSamplers) {
			continue;
		}

		auto handles = memScope.alloc<VkSampler>(bind.descriptorCount);
		for(auto i = 0u; i < bind.descriptorCount; ++i) {
			auto& sampler = get(dev, bind.pImmutableSamplers[i]);
			handles[i] = sampler.handle;
		}

		bind.pImmutableSamplers = handles.data();
	}

	auto res = dev.dispatch.CreateDescriptorSetLayout(dev.handle, &nci, nullptr, pSetLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto dsLayoutPtr = IntrusivePtr<DescriptorSetLayout>(new DescriptorSetLayout());
	auto& dsLayout = *dsLayoutPtr;
	dsLayout.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
	dsLayout.dev = &dev;
	dsLayout.handle = *pSetLayout;

	auto* flagsInfo = findChainInfo<VkDescriptorSetLayoutBindingFlagsCreateInfo,
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO>(*pCreateInfo);
	flagsInfo = (flagsInfo && flagsInfo->bindingCount == 0u) ? nullptr : flagsInfo;
	dlg_assert(!flagsInfo || flagsInfo->bindingCount == pCreateInfo->bindingCount);

	for(auto i = 0u; i < pCreateInfo->bindingCount; ++i) {
		const auto& bind = pCreateInfo->pBindings[i];
		ensureSize(dsLayout.bindings, bind.binding + 1);

		auto& dst = dsLayout.bindings[bind.binding];
		dst.binding = bind.binding;
		dst.descriptorCount = bind.descriptorCount;
		dst.descriptorType = bind.descriptorType;
		dst.stageFlags = bind.stageFlags;
		dst.flags = flagsInfo ? flagsInfo->pBindingFlags[i] : 0u;

		if(needsSampler(bind.descriptorType) && dst.descriptorCount > 0 &&
				bind.pImmutableSamplers) {
			// Couldn't find in the spec whether this is allowed or not.
			// But it seems incorrect to me, we might not handle it correctly
			// everywhere.
			dlg_assert(!(dst.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
			dst.immutableSamplers = std::make_unique<IntrusivePtr<Sampler>[]>(dst.descriptorCount);
			for(auto e = 0u; e < dst.descriptorCount; ++e) {
				dst.immutableSamplers[e] = getPtr(dev, bind.pImmutableSamplers[e]);
			}
		}
	}

	// number offsets
	auto off = 0u;
	for(auto b = 0u; b < dsLayout.bindings.size(); ++b) {
		auto& bind = dsLayout.bindings[b];
		bind.offset = off;

		off += bind.descriptorCount * descriptorSize(bind.descriptorType);

		dlg_assert(b + 1 == dsLayout.bindings.size() ||
			!(bind.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
	}

	*pSetLayout = castDispatch<VkDescriptorSetLayout>(dsLayout);
	dev.dsLayouts.mustEmplace(*pSetLayout, std::move(dsLayoutPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
		VkDevice                                    device,
		VkDescriptorSetLayout                       descriptorSetLayout,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorSetLayout) {
		return;
	}

	auto& dev = getDevice(device);
	dev.dsLayouts.mustErase(descriptorSetLayout);

	// NOTE: We intenntionally don't destruct the handle here, handle might
	// need to be kept alive, they have shared ownership. Destroyed
	// in handle destructor.
	// dev.dispatch.DestroyDescriptorSetLayout(dev.handle, dsl.handle, pAllocator);
	(void) pAllocator;
}

// dsPool
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(
		VkDevice                                    device,
		const VkDescriptorPoolCreateInfo*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorPool*                           pDescriptorPool) {
	auto& dev = getDevice(device);

	auto res = dev.dispatch.CreateDescriptorPool(dev.handle, pCreateInfo, pAllocator, pDescriptorPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto dsPoolPtr = std::make_unique<DescriptorPool>();
	auto& dsPool = *dsPoolPtr;
	dsPool.objectType = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
	dsPool.dev = &dev;
	dsPool.handle = *pDescriptorPool;
	dsPool.maxSets = pCreateInfo->maxSets;
	dsPool.poolSizes = {pCreateInfo->pPoolSizes, pCreateInfo->pPoolSizes + pCreateInfo->poolSizeCount};

	*pDescriptorPool = castDispatch<VkDescriptorPool>(dsPool);
	dev.dsPools.mustEmplace(*pDescriptorPool, std::move(dsPoolPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorPool) {
		return;
	}

	auto& dev = getDevice(device);
	auto handle = dev.dsPools.mustMove(descriptorPool)->handle;
	dev.dispatch.DestroyDescriptorPool(dev.handle, handle, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		VkDescriptorPoolResetFlags                  flags) {
	auto& dsPool = get(device, descriptorPool);
	auto& dev = *dsPool.dev;

	// We don't use a for loop since the descriptors remove themselves
	// on destruction
	while(!dsPool.descriptorSets.empty()) {
		auto* ds = dsPool.descriptorSets[0];
		if(HandleDesc<VkDescriptorSet>::wrap) {
			// TODO: ugh, this is terrible, not sure how to handle the case properly
			auto h = u64ToHandle<VkDescriptorSet>(reinterpret_cast<std::uintptr_t>(ds));
			dev.descriptorSets.mustErase(h);
		} else {
			dev.descriptorSets.mustErase(ds->handle);
		}
	}

	return dev.dispatch.ResetDescriptorPool(dev.handle, dsPool.handle, flags);
}

// descriptor set
VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(
		VkDevice                                    device,
		const VkDescriptorSetAllocateInfo*          pAllocateInfo,
		VkDescriptorSet*                            pDescriptorSets) {
	ZoneScoped;

	auto& pool = get(device, pAllocateInfo->descriptorPool);
	auto& dev = *pool.dev;

	auto nci = *pAllocateInfo;
	nci.descriptorPool = pool.handle;

	auto dsLayouts = LocalVector<VkDescriptorSetLayout>(pAllocateInfo->descriptorSetCount);
	for(auto i = 0u; i < nci.descriptorSetCount; ++i) {
		dsLayouts[i] = get(dev, pAllocateInfo->pSetLayouts[i]).handle;
	}

	nci.pSetLayouts = dsLayouts.data();

	{
		ZoneScopedN("dispatch");
		auto res = dev.dispatch.AllocateDescriptorSets(dev.handle, &nci, pDescriptorSets);
		if(res != VK_SUCCESS) {
			return res;
		}
	}

	auto* variableCountInfo = findChainInfo<VkDescriptorSetVariableDescriptorCountAllocateInfo,
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO>(*pAllocateInfo);
	if (variableCountInfo && variableCountInfo->descriptorSetCount == 0u) {
		variableCountInfo = nullptr;
	}
	dlg_assert(!variableCountInfo ||
		variableCountInfo->descriptorSetCount == pAllocateInfo->descriptorSetCount);

	for(auto i = 0u; i < pAllocateInfo->descriptorSetCount; ++i) {
		auto dsPtr = std::make_unique<DescriptorSet>();
		auto& ds = *dsPtr;
		ds.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET;
		ds.dev = &dev;
		ds.handle = pDescriptorSets[i];

		auto layoutPtr = getPtr(dev, pAllocateInfo->pSetLayouts[i]);
		auto& layout = nonNull(layoutPtr);

		// per spec variable counts are zero by default, if no other value is provided
		auto varCount = u32(0);
		if(variableCountInfo && !layout.bindings.empty() &&
				layout.bindings.back().flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
			varCount = variableCountInfo->pDescriptorCounts[i];
		}

		ds.state = newDescriptorSetState(std::move(layoutPtr), varCount);
		ds.state->ds = &ds;
		ds.pool = &pool;

		pDescriptorSets[i] = castDispatch<VkDescriptorSet>(ds);
		dev.descriptorSets.mustEmplace(pDescriptorSets[i], std::move(dsPtr));

		++dev.stats.aliveDescriptorSets;

		std::lock_guard lock(dev.mutex);
		// the application cannot access 'pool' from another thread during
		// this calls (sync guarantee per vulkan spec). But we might
		// be reading it during gui rendering at the moment (e.g. listing
		// all descriptor sets for a given pool), that's why we put
		// it inside the critical section.
		pool.descriptorSets.push_back(&ds);
		initImmutableSamplersLocked(*ds.state);
	}

	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		uint32_t                                    descriptorSetCount,
		const VkDescriptorSet*                      pDescriptorSets) {
	ZoneScoped;

	auto& pool = get(device, descriptorPool);
	auto& dev = *pool.dev;
	auto handles = LocalVector<VkDescriptorSet>(descriptorSetCount);

	for(auto i = 0u; i < descriptorSetCount; ++i) {
		auto ptr = dev.descriptorSets.mustMove(pDescriptorSets[i]);
		handles[i] = ptr->handle;
	}

	return dev.dispatch.FreeDescriptorSets(dev.handle, pool.handle,
		u32(handles.size()), handles.data());
}

void update(DescriptorSetState& state, unsigned bind, unsigned elem,
		VkBufferView& handle) {
	dlg_assert(handle);

	auto& binding = bufferViews(state, bind)[elem];
	auto ptr = getPtr(*state.layout->dev, handle);
	handle = ptr->handle;

	std::lock_guard lock(state.mutex);
	binding = std::move(ptr);
}

void update(DescriptorSetState& state, unsigned bind, unsigned elem,
		VkDescriptorImageInfo& img) {
	auto& dev = *state.layout->dev;

	auto& binding = images(state, bind)[elem];
	binding.layout = img.imageLayout;

	auto& layout = state.layout->bindings[bind];
	if(needsImageView(layout.descriptorType)) {
		dlg_assert(img.imageView);
		auto ptr = getPtr(dev, img.imageView);
		img.imageView = ptr->handle;

		std::lock_guard lock(state.mutex);
		binding.imageView = std::move(ptr);
	}

	if(needsSampler(layout.descriptorType)) {
		if(layout.immutableSamplers) {
			// immutable samplers are initialized at the beginning and
			// never unset.
			dlg_assert(binding.sampler);
			dlg_assert(binding.sampler.get() == layout.immutableSamplers[elem].get());
		} else {
			dlg_assert(img.sampler);
			auto ptr = getPtr(dev, img.sampler);
			img.sampler = ptr->handle;

			std::lock_guard lock(state.mutex);
			binding.sampler = std::move(ptr);
		}
	}
}

void update(DescriptorSetState& state, unsigned bind, unsigned elem,
		VkDescriptorBufferInfo& buf) {
	auto& binding = buffers(state, bind)[elem];
	auto ptr = getPtr(*state.layout->dev, buf.buffer);
	buf.buffer = ptr->handle;

	std::lock_guard lock(state.mutex);
	binding.buffer = std::move(ptr);
	binding.offset = buf.offset;
	binding.range = evalRange(binding.buffer->ci.size, buf.offset, buf.range);
}

void advanceUntilValid(DescriptorSetState& state, unsigned& binding, unsigned& elem) {
	dlg_assert(binding < state.layout->bindings.size());
	auto count = descriptorCount(state, binding);
	while(elem >= count) {
		++binding;
		elem = 0u;
		dlg_assert(binding < state.layout->bindings.size());
		count = descriptorCount(state, binding);
	}
}

void DescriptorSetState::PtrHandler::inc(DescriptorSetState& state) const noexcept {
	++state.refCount;
}

void DescriptorSetState::PtrHandler::dec(DescriptorSetState& state) const noexcept {
	{
		std::lock_guard lock(state.mutex);
		dlg_assert(state.refCount > 0);
		if(--state.refCount > 0) {
			return;
		}
	}

	// ref count is zero, destroy state
	// destroy bindings, mainly to release intrusive ptrs
	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		auto& binding = state.layout->bindings[b];
		if(!descriptorCount(state, b)) {
			continue;
		}

		switch(category(binding.descriptorType)) {
			case DescriptorCategory::buffer: {
				auto b = buffers(state, binding.binding);
				std::destroy(b.begin(), b.end());
				break;
			} case DescriptorCategory::bufferView: {
				auto b = bufferViews(state, binding.binding);
				std::destroy(b.begin(), b.end());
				break;
			} case DescriptorCategory::image: {
				auto b = images(state, binding.binding);
				std::destroy(b.begin(), b.end());
				break;
			} case DescriptorCategory::none:
				dlg_error("unreachable: invalid descriptor type");
				break;
		}
	}

	dlg_assert(state.layout->dev->stats.aliveDescriptorStates > 0);
	--state.layout->dev->stats.aliveDescriptorStates;

	state.~DescriptorSetState();

	// we allocated the memory as std::byte array, so we have to free
	// it like that. We don't have to call any other destructors of
	// Binding elements since they are all trivial
	auto ptr = reinterpret_cast<std::byte*>(&state);
	TracyFreeS(ptr, 8);
	delete[] ptr;
}

void checkCopyState(DescriptorSet& ds) {
	// This is called when we are updating the descriptorSets state.
	// If someone else holds a reference to to it, we need to allocate a new one.
	// This basically implements copy-on-write.
	// Edge case:
	// since we release the lock right after checking the reference count,
	// we won't correctly implement copy-on-write for copies that
	// are made *during* the following update. Fixing this isn't easily possbile
	// due to synchronization issues. But this isn't a real issue:
	// Making a copy of a state retrieved from a descriptor set should
	// only happen during submission. The only case in which another
	// thread can - at the same time - be updating a descriptor in
	// a set that is being submitted is an update_unused_while_pending
	// descriptor that would be meaningless in the referenced state anyways.

	if(ds.state->refCount == 1) {
		return;
	}

	auto newState = copyDescriptorSetState(*ds.state);
	newState->ds = &ds;

	{
		// PERF: we might get away with not involving the
		// device mutex here, just make ds.state atomic and make
		// sure we first make an IntrusivePtr copy whereever we read
		// it (probably already do this everywhere).
		// NOTE important we never hold 'ds.mutex' and 'dev.mutex' at once,
		// careful when moving code around here.
		std::lock_guard lock(ds.dev->mutex);
		ds.state = std::move(newState);
	}
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(
		VkDevice                                    device,
		uint32_t                                    descriptorWriteCount,
		const VkWriteDescriptorSet*                 pDescriptorWrites,
		uint32_t                                    descriptorCopyCount,
		const VkCopyDescriptorSet*                  pDescriptorCopies) {
	ZoneScoped;
	auto& dev = getDevice(device);

	// handle writes
	auto totalWriteCount = 0u;
	for(auto i = 0u; i < descriptorWriteCount; ++i) {
		totalWriteCount += pDescriptorWrites[i].descriptorCount;
	}

	auto writes = LocalVector<VkWriteDescriptorSet>(descriptorWriteCount);
	auto imageInfos = LocalVector<VkDescriptorImageInfo>(totalWriteCount);
	auto bufferInfos = LocalVector<VkDescriptorBufferInfo>(totalWriteCount);
	auto bufferViews = LocalVector<VkBufferView>(totalWriteCount);

	auto writeOff = 0u;
	for(auto i = 0u; i < descriptorWriteCount; ++i) {
		auto& write = pDescriptorWrites[i];
		dlg_assert(write.descriptorCount > 0u); // per vulkan spec

		auto& ds = get(dev, write.dstSet);
		dlg_assert(ds.handle);
		dlg_assert(ds.state);

		writes[i] = write;
		writes[i].dstSet = ds.handle;

		checkCopyState(ds);

		auto dstBinding = write.dstBinding;
		auto dstElem = write.dstArrayElement;
		auto invalidate = false;

		for(auto j = 0u; j < write.descriptorCount; ++j, ++dstElem) {
			advanceUntilValid(*ds.state, dstBinding, dstElem);
			auto& layout = ds.state->layout->bindings[dstBinding];
			dlg_assert(write.descriptorType == layout.descriptorType);

			if(!(layout.flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) {
				// TODO: when VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
				// is set, only invalidate the record that use the binding
				// (rules depend on whether PARTIALLY_BOUND_BIT is set as well).
				// This needs some additional tracking (probably to be done
				// while recording the record).
				// Same issue in 'copy' and 'updateWithTemplate' below
				// TODO: even though we don't invalidate the record, we
				// still need to inform it that an update_after_bind descriptor
				// changed, might need to invalidate a command hook record.
				invalidate = true;
			}

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image: {
					dlg_assert(write.pImageInfo);
					auto& info = imageInfos[writeOff + j];
					info = write.pImageInfo[j];
					update(*ds.state, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(write.pBufferInfo);
					auto& info = bufferInfos[writeOff + j];
					info = write.pBufferInfo[j];
					update(*ds.state, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::bufferView: {
					dlg_assert(write.pTexelBufferView);
					auto& info = bufferViews[writeOff + j];
					info = write.pTexelBufferView[j];
					update(*ds.state, dstBinding, dstElem, info);
					break;
				} case DescriptorCategory::none:
					dlg_error("unreachable: Invalid descriptor type");
					break;
			}
		}

		writes[i].pImageInfo = imageInfos.data() + writeOff;
		writes[i].pBufferInfo = bufferInfos.data() + writeOff;
		writes[i].pTexelBufferView = bufferViews.data() + writeOff;

		if(invalidate) {
			// TODO: not sure if needed/useful here as we don't directly
			// reference descriptor sets in a record anyways
			// ds.invalidateCbs();
		}

		writeOff += writes[i].descriptorCount;
	}

	// handle copies
	auto copies = LocalVector<VkCopyDescriptorSet>(descriptorCopyCount);

	for(auto i = 0u; i < descriptorCopyCount; ++i) {
		auto& copy = pDescriptorCopies[i];
		auto& src = get(dev, copy.srcSet);
		auto& dst = get(dev, copy.dstSet);

		copies[i] = copy;
		copies[i].srcSet = src.handle;
		copies[i].dstSet = dst.handle;

		checkCopyState(dst);

		auto dstBinding = copy.dstBinding;
		auto dstElem = copy.dstArrayElement;
		auto srcBinding = copy.srcBinding;
		auto srcElem = copy.srcArrayElement;
		auto invalidate = false;

		for(auto j = 0u; j < copy.descriptorCount; ++j, ++srcElem, ++dstElem) {
			advanceUntilValid(*dst.state, dstBinding, dstElem);
			advanceUntilValid(*src.state, srcBinding, srcElem);

			auto& layout = dst.state->layout->bindings[dstBinding];
			if(!(layout.flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) {
				// TODO: see above (VkWriteDescriptor) for the issue
				// with update_unused_while_pending
				invalidate = true;
			}

			vil::copy(*dst.state, dstBinding, dstElem, *src.state, srcBinding, srcElem);
		}

		if(invalidate) {
			// TODO: not sure if needed/useful here as we don't directly
			// reference descriptor sets in a record anyways
			// dst.invalidateCbs();
		}
	}

	{
		ZoneScopedN("dispatch.UpdateDescriptorSets");
		return dev.dispatch.UpdateDescriptorSets(dev.handle,
			u32(writes.size()), writes.data(),
			u32(copies.size()), copies.data());
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(
		VkDevice                                    device,
		const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorUpdateTemplate*                 pDescriptorUpdateTemplate) {
	// NOTE: we don't use host allocators here since this handle is potentially
	// kept alive inside the layer, preventing us from passing an application
	// allocator to the destruction function.
	// See design.md on allocators.
	(void) pAllocator;

	auto& dsLayout = get(device, pCreateInfo->descriptorSetLayout);
	auto& dev = *dsLayout.dev;
	auto& pipeLayout = get(dev, pCreateInfo->pipelineLayout);

	auto nci = *pCreateInfo;
	nci.descriptorSetLayout = dsLayout.handle;
	nci.pipelineLayout = pipeLayout.handle;

	auto res = dev.dispatch.CreateDescriptorUpdateTemplate(dev.handle, &nci,
		nullptr, pDescriptorUpdateTemplate);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto dutPtr = IntrusivePtr<DescriptorUpdateTemplate>(new DescriptorUpdateTemplate());
	auto& dut = *dutPtr;
	dut.objectType = VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE;
	dut.dev = &dev;
	dut.handle = *pDescriptorUpdateTemplate;

	dut.entries = {
		pCreateInfo->pDescriptorUpdateEntries,
		pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount
	};

	*pDescriptorUpdateTemplate = castDispatch<VkDescriptorUpdateTemplate>(dut);
	dev.dsuTemplates.mustEmplace(*pDescriptorUpdateTemplate, std::move(dutPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(
		VkDevice                                    device,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorUpdateTemplate) {
		return;
	}

	auto& dev = getDevice(device);
	dev.dsuTemplates.mustErase(descriptorUpdateTemplate);

	// Don't destroy it here, handle has shared ownership, see e.g.
	// the dsuTemplates hash map in Device for justification
	(void) pAllocator;
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(
		VkDevice                                    device,
		VkDescriptorSet                             descriptorSet,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		const void*                                 pData) {
	ZoneScoped;

	auto& ds  = get(device, descriptorSet);
	auto& dev = *ds.dev;
	auto& dut = get(dev, descriptorUpdateTemplate);

	bool invalidate = false;

	checkCopyState(ds);

	auto totalSize = totalUpdateDataSize(dut);
	auto fwdData = LocalVector<std::byte>(totalSize);
	std::memcpy(fwdData.data(), pData, totalSize);
	auto* ptr = fwdData.data();

	for(auto& entry : dut.entries) {
		auto dstBinding = entry.dstBinding;
		auto dstElem = entry.dstArrayElement;
		for(auto j = 0u; j < entry.descriptorCount; ++j, ++dstElem) {
			advanceUntilValid(*ds.state, dstBinding, dstElem);
			auto dsType = ds.state->layout->bindings[dstBinding].descriptorType;

			// TODO: such an assertion here would be nice. Track used
			// layout in update?
			// dlg_assert(write.descriptorType == type);

			auto* data = ptr + (entry.offset + j * entry.stride);

			// TODO: the reinterpret_cast here is UB in C++ I guess.
			// Assuming the caller did it correctly (really creating
			// the objects e.g. via placement new) we could probably also
			// do it correctly by using placement new (copy) into 'fwdData'
			// instead of the memcpy above.
			switch(category(dsType)) {
				case DescriptorCategory::image: {
					auto& img = *reinterpret_cast<VkDescriptorImageInfo*>(data);
					update(*ds.state, dstBinding, dstElem, img);
					break;
				} case DescriptorCategory::buffer: {
					auto& buf = *reinterpret_cast<VkDescriptorBufferInfo*>(data);
					update(*ds.state, dstBinding, dstElem, buf);
					break;
				} case DescriptorCategory::bufferView: {
					auto& bufView = *reinterpret_cast<VkBufferView*>(data);
					update(*ds.state, dstBinding, dstElem, bufView);
					break;
				} case DescriptorCategory::none:
					dlg_error("Invalid/unknown descriptor type");
					break;
			}
		}
	}

	if(invalidate) {
		// TODO: not sure if needed/useful here as we don't directly
		// reference descriptor sets in a record anyways
		// ds.invalidateCbs();
	}

	{
		ZoneScopedN("dispatch.UpdateDescriptorSetWithTemplate");
		dev.dispatch.UpdateDescriptorSetWithTemplate(dev.handle, ds.handle,
			dut.handle, static_cast<const void*>(fwdData.data()));
	}
}

u32 totalUpdateDataSize(const DescriptorUpdateTemplate& dut) {
	u32 ret = 0u;
	for(auto& entry : dut.entries) {
		auto off = u32(entry.offset + entry.descriptorCount * entry.stride);
		switch(category(entry.descriptorType)) {
			case DescriptorCategory::image:
				off += sizeof(VkDescriptorImageInfo);
				break;
			case DescriptorCategory::buffer:
				off += sizeof(VkDescriptorBufferInfo);
				break;
			case DescriptorCategory::bufferView:
				off += sizeof(VkBufferView*);
				break;
			case DescriptorCategory::none:
				dlg_error("unreachable: Invalid/unknown descriptor type");
				break;
		}

		ret = std::max<u32>(ret, off);
	}

	return ret;
}

} // namespace vil
