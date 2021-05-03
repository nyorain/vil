#include <ds.hpp>
#include <device.hpp>
#include <data.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <util/util.hpp>

namespace vil {

// util
size_t totalNumBindings(const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	dlg_assert(!layout.bindings.empty());
	auto& last = layout.bindings.back();
	size_t ret = last.offset;

	if(last.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		ret += variableDescriptorCount;
	} else {
		ret += last.descriptorCount;
	}

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

void unregisterLocked(DescriptorSetState& state, unsigned bindingID,
		unsigned elemID, bool unregisterStaticSampler = false) {
	auto removeFromHandle = [&](auto& handle) {
		DescriptorStateRef ref = {&state, bindingID, elemID};
		auto it = handle.descriptors.find(ref);
		dlg_assert(it != handle.descriptors.end());
		handle.descriptors.erase(it);
	};

	auto& bind = binding(state, bindingID, elemID);

	auto& bindingLayout = state.layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	switch(category(dsType)) {
		case DescriptorCategory::buffer: {
			dlg_assert(bind.valid);
			removeFromHandle(nonNull(bind.bufferInfo.buffer));
			bind.bufferInfo = {};
			break;
		} case DescriptorCategory::image: {
			// may not be true, e.g. if first the sampler is destroyed
			// and unsets both, then the image view is destroyed.
			// dlg_assert(bind.imageInfo.imageView || bind.imageInfo.sampler);
			if(bind.imageInfo.imageView) {
				dlg_assert(bind.valid);
				removeFromHandle(*bind.imageInfo.imageView);
				bind.imageInfo.imageView = {};
				bind.imageInfo.layout = {};
			}

			if(bind.imageInfo.sampler &&
					(!bindingLayout.immutableSamplers.get() || unregisterStaticSampler)) {
				dlg_assert(bind.valid || unregisterStaticSampler);
				removeFromHandle(*bind.imageInfo.sampler);
				bind.imageInfo.sampler = {};
			}

			break;
		} case DescriptorCategory::bufferView: {
			dlg_assert(bind.valid);
			removeFromHandle(nonNull(bind.bufferView));
			bind.bufferView = {};
			break;
		} default: dlg_error("Unimplemented descriptor type"); break;
	}

	bind.valid = false;
}

void notifyDestroyLocked(DescriptorSetState& state, unsigned binding, unsigned elem,
		const Handle& handle) {

	unregisterLocked(state, binding, elem, handle.objectType == VK_OBJECT_TYPE_SAMPLER);
	if(state.ds) {
		auto& layout = state.layout->bindings[binding];

		// TODO: I couldn't find in the spec what happens in this situation:
		// a view/sampler/buffer bound to a binding with update_after_bind
		// was destroyed. Will command buffers using that ds be invalidated?
		// My intuition says it won't be invalidated, could be updated
		// later on with valid view again, the order (update binding or
		// destroy view first) should really not matter. But the spec does not
		// explicitly state this, so not sure.
		if(!(layout.flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) {
			// TODO: also respect the other flags (e.g. update_unused_while_pending)
			// see notes in other invalidation places.
			state.ds->invalidateCbsLocked();
		}
	}
}

DescriptorSetStatePtr newDescriptorSetState(
		IntrusivePtr<DescriptorSetLayout> layout, u32 variableDescriptorCount) {
	auto numBindings = totalNumBindings(*layout, variableDescriptorCount);
	auto memSize = sizeof(DescriptorSetState) +
		numBindings * sizeof(DescriptorBinding);
	auto mem = new std::byte[memSize]();

	auto* state = new(mem) DescriptorSetState();
	state->layout = std::move(layout);
	state->variableDescriptorCount = variableDescriptorCount;

	auto* base = mem + sizeof(DescriptorSetState);
	auto* bindings = reinterpret_cast<DescriptorBinding*>(base);
	new(bindings) DescriptorBinding[numBindings]();

	return DescriptorSetStatePtr(state);
}

void copyLocked(DescriptorSetState& dst, unsigned dstBindID, unsigned dstElemID,
		const DescriptorSetState& src, unsigned srcBindID, unsigned srcElemID) {
	auto& srcBind = binding(src, srcBindID, srcElemID);
	auto& dstBind = binding(dst, dstBindID, dstElemID);
	if(!srcBind.valid) {
		return;
	}

	auto& srcLayout = src.layout->bindings[srcBindID];
	auto& dstLayout = dst.layout->bindings[dstBindID];
	dlg_assert(srcLayout.descriptorType == dstLayout.descriptorType);

	if(dstBind.valid) {
		unregisterLocked(dst, dstBindID, dstElemID);
	}

	switch(category(dstLayout.descriptorType)) {
		case DescriptorCategory::image: {
			dstBind.imageInfo = {};
			dstBind.imageInfo.layout = srcBind.imageInfo.layout;

			if(srcBind.imageInfo.imageView) {
				dstBind.imageInfo.imageView = srcBind.imageInfo.imageView;
				dstBind.imageInfo.imageView->descriptors.insert({&dst, dstBindID, dstElemID});
			}
			if(srcBind.imageInfo.sampler && !dstLayout.immutableSamplers.get()) {
				dstBind.imageInfo.sampler = srcBind.imageInfo.sampler;
				dstBind.imageInfo.sampler->descriptors.insert({&dst, dstBindID, dstElemID});
			}
			break;
		} case DescriptorCategory::buffer: {
			dlg_assert(srcBind.bufferInfo.buffer);
			dstBind.bufferInfo = srcBind.bufferInfo;
			dstBind.bufferInfo.buffer->descriptors.insert({&dst, dstBindID, dstElemID});
			break;
		} case DescriptorCategory::bufferView:
			dlg_assert(srcBind.bufferView);
			dstBind.bufferView = srcBind.bufferView;
			dstBind.bufferView->descriptors.insert({&dst, dstBindID, dstElemID});
			break;
		default: break;
	}

	dstBind.valid = true;
}

void initImmutableSamplersLocked(DescriptorSetState& state) {
	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		// If the binding holds immutable samplers, fill them in.
		// We do this so we don't have to check for immutable samplers
		// every time we read a binding. Also needed for correct
		// invalidation tracking.
		if(state.layout->bindings[b].immutableSamplers.get()) {
			dlg_assert(needsSampler(state.layout->bindings[b].descriptorType));
			auto binds = bindings(state, b);

			for(auto e = 0u; e < binds.size(); ++e) {
				auto sampler = state.layout->bindings[b].immutableSamplers[e];
				dlg_assert(sampler);

				binds[e].imageInfo.sampler = sampler;

				// when the bindings contains only a sampler, it's already valid.
				if(state.layout->bindings[b].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
					binds[e].valid = true;
				}

				sampler->descriptors.insert({&state, b, e});
			}
		}
	}
}

DescriptorSetStatePtr copyDescriptorSetStateLocked(const DescriptorSetState& state) {
	dlg_assert(state.layout->dev->mutex.owned());
	auto ret = newDescriptorSetState(state.layout, state.variableDescriptorCount);

	initImmutableSamplersLocked(*ret);

	// copy descriptors
	for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
		for(auto e = 0u; e < descriptorCount(state, b); ++e) {
			copyLocked(*ret, b, e, state, b, e);
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

void DescriptorSetState::Deleter::operator()(DescriptorSetState* state) const {
	// call destructor manually
	state->~DescriptorSetState();

	// we allocated the memory as std::byte array, so we have to free
	// it like that. We don't have to call any other destructors of
	// Binding elements since they are all trivial
	auto ptr = reinterpret_cast<std::byte*>(state);
	delete[] ptr;
}

DescriptorSetState::~DescriptorSetState() {
	dlg_assert(layout);
	auto& dev = layout->dev;

	std::lock_guard lock(dev->mutex);

	for(auto b = 0u; b < layout->bindings.size(); ++b) {
		auto bs = bindings(*this, b);
		for(auto e = 0u; e < bs.size(); ++e) {
			if(!bs[e].valid && !layout->bindings[b].immutableSamplers.get()) {
				continue;
			}

			unregisterLocked(*this, b, e, true);
		}
	}
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
}

Sampler* getSampler(DescriptorSetState& state, unsigned bindingID, unsigned elemID) {
	dlg_assert(state.layout->bindings.size() > bindingID);
	dlg_assert(needsSampler(state.layout->bindings[bindingID].descriptorType));
	auto& binding = vil::binding(state, bindingID, elemID);
	return binding.imageInfo.sampler;
}

ImageView* getImageView(DescriptorSetState& state, unsigned bindingID, unsigned elemID) {
	dlg_assert(state.layout->bindings.size() > bindingID);
	dlg_assert(needsImageView(state.layout->bindings[bindingID].descriptorType));
	auto& binding = vil::binding(state, bindingID, elemID);
	return binding.imageInfo.imageView;
}

Buffer* getBuffer(DescriptorSetState& state, unsigned bindingID, unsigned elemID) {
	dlg_assert(state.layout->bindings.size() > bindingID);
	dlg_assert(category(state.layout->bindings[bindingID].descriptorType)
		== DescriptorCategory::buffer);
	auto& binding = vil::binding(state, bindingID, elemID);
	return binding.bufferInfo.buffer;
}

BufferView* getBufferView(DescriptorSetState& state, unsigned bindingID, unsigned elemID) {
	dlg_assert(state.layout->bindings.size() > bindingID);
	dlg_assert(category(state.layout->bindings[bindingID].descriptorType)
		== DescriptorCategory::bufferView);
	auto& binding = vil::binding(state, bindingID, elemID);
	return binding.bufferView;
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
		auto* ds = descriptorSets[0];
		dev->descriptorSets.mustErase(ds->handle);
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
			return {};
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

	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateDescriptorSetLayout(device, pCreateInfo, nullptr, pSetLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dsLayout = dev.dsLayouts.add(*pSetLayout);
	dsLayout.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
	dsLayout.dev = &dev;
	dsLayout.handle = *pSetLayout;

	auto* flagsInfo = findChainInfo<VkDescriptorSetLayoutBindingFlagsCreateInfo,
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO>(*pCreateInfo);
	flagsInfo = (flagsInfo && flagsInfo->bindingCount == 0u) ? nullptr : flagsInfo;
	dlg_assert(!flagsInfo || flagsInfo->bindingCount == pCreateInfo->bindingCount);

	u32 off = 0u;
	for(auto i = 0u; i < pCreateInfo->bindingCount; ++i) {
		const auto& bind = pCreateInfo->pBindings[i];
		ensureSize(dsLayout.bindings, bind.binding + 1);

		auto& dst = dsLayout.bindings[bind.binding];
		dst.binding = bind.binding;
		dst.descriptorCount = bind.descriptorCount;
		dst.descriptorType = bind.descriptorType;
		dst.stageFlags = bind.stageFlags;
		dst.flags = flagsInfo ? flagsInfo->pBindingFlags[i] : 0u;
		dst.offset = off;

		if(needsSampler(bind.descriptorType) && bind.pImmutableSamplers) {
			// Couldn't find in the spec whether this is allowed or not.
			// But it seems incorrect to me, we might not handle it correctly
			// everywhere.
			dlg_assert(!(dst.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT));
			dst.immutableSamplers = std::make_unique<Sampler*[]>(dst.descriptorCount);
			for(auto e = 0u; e < dst.descriptorCount; ++e) {
				dst.immutableSamplers[e] = &dev.samplers.get(bind.pImmutableSamplers[e]);
			}
		}

		off += bind.descriptorCount;
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
		VkDevice                                    device,
		VkDescriptorSetLayout                       descriptorSetLayout,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorSetLayout) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.dsLayouts.mustErase(descriptorSetLayout);

	// NOTE: We intenntionally don't destruct the handle here, handle might
	// need to be kept alive, they have shared ownership. Destroyed
	// in handle destructor.
	// dev.dispatch.DestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator);
	(void) pAllocator;
}

// dsPool
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(
		VkDevice                                    device,
		const VkDescriptorPoolCreateInfo*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorPool*                           pDescriptorPool) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dsPool = dev.dsPools.add(*pDescriptorPool);
	dsPool.objectType = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
	dsPool.dev = &dev;
	dsPool.handle = *pDescriptorPool;
	dsPool.maxSets = pCreateInfo->maxSets;
	dsPool.poolSizes = {pCreateInfo->pPoolSizes, pCreateInfo->pPoolSizes + pCreateInfo->poolSizeCount};

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		const VkAllocationCallbacks*                pAllocator) {
	if(!descriptorPool) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.dsPools.mustErase(descriptorPool);
	dev.dispatch.DestroyDescriptorPool(device, descriptorPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		VkDescriptorPoolResetFlags                  flags) {
	auto& dev = getData<Device>(device);
	auto& dsPool = dev.dsPools.get(descriptorPool);

	// We don't use a for loop since the descriptors remove themselves
	// on destruction
	while(!dsPool.descriptorSets.empty()) {
		auto* ds = dsPool.descriptorSets[0];
		dev.descriptorSets.mustErase(ds->handle);
	}

	return dev.dispatch.ResetDescriptorPool(device, descriptorPool, flags);
}

// descriptor set
VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(
		VkDevice                                    device,
		const VkDescriptorSetAllocateInfo*          pAllocateInfo,
		VkDescriptorSet*                            pDescriptorSets) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.AllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& pool = dev.dsPools.get(pAllocateInfo->descriptorPool);

	auto* variableCountInfo = findChainInfo<VkDescriptorSetVariableDescriptorCountAllocateInfo,
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO>(*pAllocateInfo);
	if (variableCountInfo && variableCountInfo->descriptorSetCount == 0u) {
		variableCountInfo = nullptr;
	}
	dlg_assert(!variableCountInfo ||
		variableCountInfo->descriptorSetCount == pAllocateInfo->descriptorSetCount);

	for(auto i = 0u; i < pAllocateInfo->descriptorSetCount; ++i) {
		auto& ds = dev.descriptorSets.add(pDescriptorSets[i]);
		ds.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET;
		ds.dev = &dev;
		ds.handle = pDescriptorSets[i];

		auto layoutPtr = dev.dsLayouts.getPtr(pAllocateInfo->pSetLayouts[i]);
		auto& layout = *layoutPtr;
		dlg_assert(!layout.bindings.empty());

		// per spec variable counts are zero by default, if no other value is provided
		auto varCount = u32(0);
		if(layout.bindings.back().flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
			varCount = variableCountInfo->pDescriptorCounts[i];
		}

		ds.state = newDescriptorSetState(std::move(layoutPtr), varCount);
		ds.state->ds = &ds;
		ds.pool = &pool;

		std::lock_guard lock(dev.mutex);
		// the application cannot access 'pool' from another thread during
		// this calls (sync guarantee per vulkan spec). But we might
		// be reading it during gui rendering at the moment (e.g. listing
		// all descriptor sets for a given pool), that's why we put
		// it inside the critical section.
		pool.descriptorSets.push_back(&ds);
		initImmutableSamplersLocked(*ds.state);
	}

	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(
		VkDevice                                    device,
		VkDescriptorPool                            descriptorPool,
		uint32_t                                    descriptorSetCount,
		const VkDescriptorSet*                      pDescriptorSets) {
	auto& dev = getData<Device>(device);

	for(auto i = 0u; i < descriptorSetCount; ++i) {
		dev.descriptorSets.mustErase(pDescriptorSets[i]);
	}

	return dev.dispatch.FreeDescriptorSets(device, descriptorPool,
		descriptorSetCount, pDescriptorSets);
}

void updateLocked(DescriptorSetState& state, DescriptorBinding& binding,
		unsigned bind, unsigned elem, VkBufferView bufferView) {
	dlg_assert(bufferView);
	binding.bufferView = &state.layout->dev->bufferViews.getLocked(bufferView);
	binding.bufferView->descriptors.insert({&state, bind, elem});
	binding.valid = true;
}

void updateLocked(DescriptorSetState& state, DescriptorBinding& binding,
		unsigned bind, unsigned elem, const VkDescriptorImageInfo& img) {
	auto* dev = state.layout->dev;
	binding.imageInfo.layout = img.imageLayout;

	auto& layout = state.layout->bindings[bind];
	if(needsImageView(layout.descriptorType)) {
		dlg_assert(img.imageView);
		binding.imageInfo.imageView = &dev->imageViews.getLocked(img.imageView);
		binding.imageInfo.imageView->descriptors.insert({&state, bind, elem});
	}

	if(needsSampler(layout.descriptorType)) {
		if(layout.immutableSamplers) {
			// immutable samplers are initialized at the beginning and
			// never unset.
			dlg_assert(binding.imageInfo.sampler);
			dlg_assert(binding.imageInfo.sampler == layout.immutableSamplers[elem]);
		} else {
			binding.imageInfo.sampler = &dev->samplers.getLocked(img.sampler);
			dlg_assert(binding.imageInfo.sampler);
			binding.imageInfo.sampler->descriptors.insert({&state, bind, elem});
		}
	}

	binding.valid = true;
}

void updateLocked(DescriptorSetState& state, DescriptorBinding& binding,
		unsigned bind, unsigned elem, const VkDescriptorBufferInfo& buf) {
	binding.bufferInfo.buffer = &state.layout->dev->buffers.getLocked(buf.buffer);
	binding.bufferInfo.buffer->descriptors.insert({&state, bind, elem});
	binding.bufferInfo.offset = buf.offset;
	binding.bufferInfo.range = buf.range;
	binding.valid = true;
}

DescriptorBinding& advanceUntilValid(DescriptorSetState& state,
		unsigned& binding, unsigned& elem) {
	dlg_assert(binding < state.layout->bindings.size());
	auto binds = bindings(state, binding);
	while(elem >= binds.size()) {
		++binding;
		elem = 0u;
		dlg_assert(binding < state.layout->bindings.size());
		binds = bindings(state, binding);
	}

	return binds[elem];
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(
		VkDevice                                    device,
		uint32_t                                    descriptorWriteCount,
		const VkWriteDescriptorSet*                 pDescriptorWrites,
		uint32_t                                    descriptorCopyCount,
		const VkCopyDescriptorSet*                  pDescriptorCopies) {
	auto& dev = getData<Device>(device);

	// handle writes
	for(auto i = 0u; i < descriptorWriteCount; ++i) {
		auto& write = pDescriptorWrites[i];
		dlg_assert(write.descriptorCount > 0u); // per vulkan spec

		auto& ds = dev.descriptorSets.get(write.dstSet);
		dlg_assert(ds.handle);

		// The whole update process must be atomic
		std::lock_guard lock(dev.mutex);

		// We are updating the descriptorSets state, if someone
		// else holds on to it, we need to allocate a new one.
		// This basically implements copy-on-write
		if(ds.state->refCount > 1) {
			ds.state->ds = nullptr;
			ds.state = copyDescriptorSetStateLocked(*ds.state);
			ds.state->ds = &ds;
		}

		auto dstBinding = write.dstBinding;
		auto dstElem = write.dstArrayElement;
		auto invalidate = false;

		for(auto j = 0u; j < write.descriptorCount; ++j, ++dstElem) {
			auto& binding = advanceUntilValid(*ds.state, dstBinding, dstElem);
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

			if(binding.valid) {
				unregisterLocked(*ds.state, dstBinding, dstElem);
			}

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image: {
					dlg_assert(write.pImageInfo);
					updateLocked(*ds.state, binding, dstBinding, dstElem, write.pImageInfo[j]);
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(write.pBufferInfo);
					updateLocked(*ds.state, binding, dstBinding, dstElem, write.pBufferInfo[j]);
					break;
				} case DescriptorCategory::bufferView:
					dlg_assert(write.pTexelBufferView);
					updateLocked(*ds.state, binding, dstBinding, dstElem, write.pTexelBufferView[j]);
					break;
				default: break;
			}
		}

		if(invalidate) {
			ds.invalidateCbsLocked();
		}
	}

	// handle copies
	for(auto i = 0u; i < descriptorCopyCount; ++i) {
		auto& copy = pDescriptorCopies[i];
		auto& src = dev.descriptorSets.get(copy.srcSet);
		auto& dst = dev.descriptorSets.get(copy.dstSet);

		// The whole update process must be atomic
		std::lock_guard lock(dev.mutex);

		// We are updating the descriptorSets state, if someone
		// else holds on to it, we need to allocate a new one.
		// This basically implements copy-on-write
		if(dst.state->refCount > 1) {
			dst.state->ds = nullptr;
			dst.state = copyDescriptorSetStateLocked(*dst.state);
			dst.state->ds = &dst;
		}

		auto dstBinding = copy.dstBinding;
		auto dstElem = copy.dstArrayElement;
		auto srcBinding = copy.srcBinding;
		auto srcElem = copy.srcArrayElement;
		auto invalidate = false;

		for(auto j = 0u; j < copy.descriptorCount; ++j, ++srcElem, ++dstElem) {
			auto& dstBind = advanceUntilValid(*dst.state, dstBinding, dstElem);
			auto& srcBind = advanceUntilValid(*src.state, srcBinding, srcElem);

			dlg_assert(srcBind.valid);
			(void) dstBind;

			auto& layout = dst.state->layout->bindings[dstBinding];
			if(!(layout.flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) {
				// TODO: see above (VkWriteDescriptor) for the issue
				// with update_unused_while_pending
				invalidate = true;
			}

			copyLocked(*dst.state, dstBinding, dstElem, *src.state, srcBinding, srcElem);
		}

		if(invalidate) {
			dst.invalidateCbsLocked();
		}
	}

	return dev.dispatch.UpdateDescriptorSets(device,
		descriptorWriteCount, pDescriptorWrites,
		descriptorCopyCount, pDescriptorCopies);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(
		VkDevice                                    device,
		const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorUpdateTemplate*                 pDescriptorUpdateTemplate) {
	auto& dev = getData<Device>(device);

	auto f = dev.dispatch.CreateDescriptorUpdateTemplate;
	auto res = f(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dut = dev.dsuTemplates.add(*pDescriptorUpdateTemplate);
	dut.objectType = VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE;
	dut.dev = & dev;
	dut.handle = *pDescriptorUpdateTemplate;

	dut.entries = {
		pCreateInfo->pDescriptorUpdateEntries,
		pCreateInfo->pDescriptorUpdateEntries + pCreateInfo->descriptorUpdateEntryCount
	};

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(
		VkDevice                                    device,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dsuTemplates.mustErase(descriptorUpdateTemplate);

	auto f = dev.dispatch.DestroyDescriptorUpdateTemplate;
	f(device, descriptorUpdateTemplate, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(
		VkDevice                                    device,
		VkDescriptorSet                             descriptorSet,
		VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
		const void*                                 pData) {
	auto& dev = getData<Device>(device);
	auto& ds  = dev.descriptorSets.get(descriptorSet);
	auto& dut = dev.dsuTemplates.get(descriptorUpdateTemplate);

	auto* ptr = static_cast<const std::byte*>(pData);
	bool invalidate = false;

	{
		// The whole update process must be atomic
		std::lock_guard lock(dev.mutex);

		// We are updating the descriptorSets state, if someone
		// else holds on to it, we need to allocate a new one.
		// This basically implements copy-on-write
		if(ds.state->refCount > 1) {
			ds.state->ds = nullptr;
			ds.state = copyDescriptorSetStateLocked(*ds.state);
			ds.state->ds = &ds;
		}

		for(auto& entry : dut.entries) {
			auto dstBinding = entry.dstBinding;
			auto dstElem = entry.dstArrayElement;
			for(auto j = 0u; j < entry.descriptorCount; ++j, ++dstElem) {
				auto& binding = advanceUntilValid(*ds.state, dstBinding, dstElem);
				auto dsType = ds.state->layout->bindings[dstBinding].descriptorType;

				// NOTE: such an assertion here would be nice. Track used
				// layout in update?
				// dlg_assert(write.descriptorType == type);

				auto* data = ptr + (entry.offset + j * entry.stride);

				switch(category(dsType)) {
					case DescriptorCategory::image: {
						auto& img = *reinterpret_cast<const VkDescriptorImageInfo*>(data);
						updateLocked(*ds.state, binding, dstBinding, dstElem, img);
						break;
					} case DescriptorCategory::buffer: {
						auto& buf = *reinterpret_cast<const VkDescriptorBufferInfo*>(data);
						updateLocked(*ds.state, binding, dstBinding, dstElem, buf);
						break;
					} case DescriptorCategory::bufferView: {
						auto& bufView = *reinterpret_cast<const VkBufferView*>(data);
						updateLocked(*ds.state, binding, dstBinding, dstElem, bufView);
						break;
					} default:
						dlg_error("Invalid/unknown descriptor type");
						break;
				}
			}
		}
	}

	if(invalidate) {
		ds.invalidateCbs();
	}

	auto f = dev.dispatch.UpdateDescriptorSetWithTemplate;
	f(device, descriptorSet, descriptorUpdateTemplate, pData);
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
			default:
				dlg_error("Invalid/unknown descriptor type");
				break;
		}

		ret = std::max<u32>(ret, off);
	}

	return ret;
}

} // namespace vil
