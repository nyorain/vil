#include <ds.hpp>
#include <device.hpp>
#include <data.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <util/util.hpp>

namespace vil {

// Classes
void unregisterLocked(DescriptorSet& ds, unsigned binding, unsigned elem) {
	dlg_assert(ds.dev);
	dlg_assert(ds.bindings.size() > binding);
	dlg_assert(ds.bindings[binding].size() > elem);

	auto& bind = ds.bindings[binding][elem];
	dlg_assert(bind.valid);

	auto removeFromHandle = [&](auto& handle) {
		DescriptorSetRef ref = {&ds, binding, elem};
		auto it = handle.descriptors.find(ref);
		dlg_assert(it != handle.descriptors.end());
		handle.descriptors.erase(it);
	};

	auto& bindingLayout = ds.layout->bindings[binding];
	auto dsType = bindingLayout.descriptorType;
	switch(category(dsType)) {
		case DescriptorCategory::buffer: {
			removeFromHandle(nonNull(bind.bufferInfo.buffer));
			break;
		} case DescriptorCategory::image: {
			dlg_assert(bind.imageInfo.imageView || bind.imageInfo.sampler);
			if(bind.imageInfo.imageView) {
				removeFromHandle(*bind.imageInfo.imageView);
			}

			if(bind.imageInfo.sampler) {
				removeFromHandle(*bind.imageInfo.sampler);
			}

			break;
		} case DescriptorCategory::bufferView: {
			removeFromHandle(nonNull(bind.bufferView));
			break;
		} default: dlg_error("Unimplemented descriptor type"); break;
	}

	bind = {};
}

DescriptorSet::~DescriptorSet() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	for(auto b = 0u; b < bindings.size(); ++b) {
		for(auto e = 0u; e < bindings[b].size(); ++e) {
			if(!bindings[b][e].valid) {
				continue;
			}

			unregisterLocked(*this, b, e);
		}
	}

	// Remove from descriptor pool.
	// Pools can't be destroyed before their sets as they implicitly free them.
	dlg_assert(pool);

	auto it = find(pool->descriptorSets, this);
	dlg_assert(it != pool->descriptorSets.end());
	pool->descriptorSets.erase(it);
}

Sampler* DescriptorSet::getSampler(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::image);
	return bindings[binding][elem].imageInfo.sampler;
}

ImageView* DescriptorSet::getImageView(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::image);
	return bindings[binding][elem].imageInfo.imageView;
}

Buffer* DescriptorSet::getBuffer(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::buffer);
	return bindings[binding][elem].bufferInfo.buffer;
}

BufferView* DescriptorSet::getBufferView(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::bufferView);
	return bindings[binding][elem].bufferView;
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

	for(auto i = 0u; i < pCreateInfo->bindingCount; ++i) {
		const auto& bind = pCreateInfo->pBindings[i];
		ensureSize(dsLayout.bindings, bind.binding + 1);

		auto& dst = dsLayout.bindings[bind.binding];
		dst.binding = bind.binding;
		dst.descriptorCount = bind.descriptorCount;
		dst.descriptorType = bind.descriptorType;
		dst.stageFlags = bind.stageFlags;

		if(needsSampler(bind.descriptorType) && bind.pImmutableSamplers) {
			dst.immutableSamplers = std::make_unique<Sampler*[]>(dst.descriptorCount);
			for(auto e = 0u; e < dst.descriptorCount; ++e) {
				dst.immutableSamplers[e] = &dev.samplers.get(bind.pImmutableSamplers[e]);
			}
		}
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
	for(auto i = 0u; i < pAllocateInfo->descriptorSetCount; ++i) {
		auto& ds = dev.descriptorSets.add(pDescriptorSets[i]);
		ds.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET;
		ds.dev = &dev;
		ds.handle = pDescriptorSets[i];
		ds.layout = dev.dsLayouts.getPtr(pAllocateInfo->pSetLayouts[i]);
		ds.pool = &pool;

		ds.bindings.resize(ds.layout->bindings.size());
		for(auto b = 0u; b < ds.bindings.size(); ++b) {
			ds.bindings[b].resize(ds.layout->bindings[b].descriptorCount);

			// If the binding holds immutable samplers, fill them in.
			// We do this so we don't have to check for immutable samplers
			// every time we read a binding. Also needed for correct
			// invalidation tracking.
			if(ds.layout->bindings[b].immutableSamplers.get()) {
				dlg_assert(needsSampler(ds.layout->bindings[b].descriptorType));
				for(auto e = 0u; e < ds.bindings[b].size(); ++e) {
					auto sampler = ds.layout->bindings[b].immutableSamplers[e];
					dlg_assert(sampler);

					ds.bindings[b][e].imageInfo.sampler = sampler;
					ds.bindings[b][e].valid = true;
					sampler->descriptors.insert({&ds, b, e});
				}
			}
		}

		pool.descriptorSets.push_back(&ds);
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

void updateLocked(DescriptorSet& ds, DescriptorSet::Binding& binding,
		unsigned bind, unsigned elem, VkBufferView bufferView) {
	dlg_assert(bufferView);
	binding.bufferView = &ds.dev->bufferViews.getLocked(bufferView);
	binding.bufferView->descriptors.insert({&ds, bind, elem});
	binding.valid = true;
}

void updateLocked(DescriptorSet& ds, DescriptorSet::Binding& binding,
		unsigned bind, unsigned elem, const VkDescriptorImageInfo& img) {
	binding.imageInfo.layout = img.imageLayout;

	auto& layout = ds.layout->bindings[bind];
	if(needsImageView(layout.descriptorType)) {
		dlg_assert(img.imageView);
		binding.imageInfo.imageView = &ds.dev->imageViews.getLocked(img.imageView);
		binding.imageInfo.imageView->descriptors.insert({&ds, bind, elem});
	}

	if(needsSampler(layout.descriptorType)) {
		// Even when we have an immutable sampler here, we still add it
		// to the binding and add this ds as reference for the given sampler.
		// This is needed so that this descriptor gets properly invalidated
		// when the immutable sampler is destroyed.
		if(layout.immutableSamplers) {
			binding.imageInfo.sampler = layout.immutableSamplers[elem];
		} else {
			binding.imageInfo.sampler = &ds.dev->samplers.getLocked(img.sampler);
		}

		dlg_assert(binding.imageInfo.sampler);
		binding.imageInfo.sampler->descriptors.insert({&ds, bind, elem});
	}
	binding.valid = true;
}

void updateLocked(DescriptorSet& ds, DescriptorSet::Binding& binding,
		unsigned bind, unsigned elem, const VkDescriptorBufferInfo& buf) {
	binding.bufferInfo.buffer = &ds.dev->buffers.getLocked(buf.buffer);
	binding.bufferInfo.buffer->descriptors.insert({&ds, bind, elem});
	binding.bufferInfo.offset = buf.offset;
	binding.bufferInfo.range = buf.range;
	binding.valid = true;
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
		auto& ds = dev.descriptorSets.get(write.dstSet);

		auto dstBinding = write.dstBinding;
		auto dstElem = write.dstArrayElement;
		for(auto j = 0u; j < write.descriptorCount; ++j, ++dstElem) {
			dlg_assert(dstBinding < ds.layout->bindings.size());
			while(dstElem >= ds.bindings[dstBinding].size()) {
				++dstBinding;
				dstElem = 0u;
				dlg_assert(dstBinding < ds.layout->bindings.size());
			}

			dlg_assert(write.descriptorType == ds.layout->bindings[dstBinding].descriptorType);
			auto& binding = ds.bindings[dstBinding][dstElem];

			std::lock_guard lock(dev.mutex);
			if(binding.valid) {
				unregisterLocked(ds, dstBinding, dstElem);
			}

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image: {
					dlg_assert(write.pImageInfo);
					updateLocked(ds, binding, dstBinding, dstElem, write.pImageInfo[j]);
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(write.pBufferInfo);
					updateLocked(ds, binding, dstBinding, dstElem, write.pBufferInfo[j]);
					break;
				} case DescriptorCategory::bufferView:
					dlg_assert(write.pTexelBufferView);
					updateLocked(ds, binding, dstBinding, dstElem, write.pTexelBufferView[j]);
					break;
				default: break;
			}
		}

		// TODO: change/check for descriptor indexing
		ds.invalidateCbs();
	}

	// handle copies
	for(auto i = 0u; i < descriptorCopyCount; ++i) {
		auto& copy = pDescriptorCopies[i];
		auto& src = dev.descriptorSets.get(copy.srcSet);
		auto& dst = dev.descriptorSets.get(copy.dstSet);

		auto dstBinding = copy.dstBinding;
		auto dstElem = copy.dstArrayElement;
		auto srcBinding = copy.srcBinding;
		auto srcElem = copy.srcArrayElement;
		for(auto j = 0u; j < copy.descriptorCount; ++j, ++srcElem, ++dstElem) {
			dlg_assert(dstBinding < dst.layout->bindings.size());
			while(dstElem >= dst.bindings[dstBinding].size()) {
				++dstBinding;
				dstElem = 0u;
				dlg_assert(dstBinding < dst.layout->bindings.size());
			}

			dlg_assert(srcBinding < src.layout->bindings.size());
			while(srcElem >= src.bindings[srcBinding].size()) {
				++srcBinding;
				srcElem = 0u;
				dlg_assert(srcBinding < src.layout->bindings.size());
			}

			auto& srcBind = src.bindings[srcBinding][srcElem];
			auto& dstBind = dst.bindings[dstBinding][dstElem];
			dlg_assert(srcBind.valid);
			dlg_assert(dst.layout->bindings[dstBinding].descriptorType ==
				src.layout->bindings[dstBinding].descriptorType);

			std::lock_guard lock(dev.mutex);
			if(dstBind.valid) {
				unregisterLocked(dst, dstBinding, dstElem);
			}

			dstBind = srcBind;
			switch(category(dst.layout->bindings[dstBinding].descriptorType)) {
				case DescriptorCategory::image: {
					if(dstBind.imageInfo.imageView) {
						dstBind.imageInfo.imageView->descriptors.insert({&dst, dstBinding, dstElem});
					}
					if(dstBind.imageInfo.sampler) {
						dstBind.imageInfo.sampler->descriptors.insert({&dst, dstBinding, dstElem});
					}
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(dstBind.bufferInfo.buffer);
					dstBind.bufferInfo.buffer->descriptors.insert({&dst, dstBinding, dstElem});
					break;
				} case DescriptorCategory::bufferView:
					dlg_assert(dstBind.bufferView);
					dstBind.bufferView->descriptors.insert({&dst, dstBinding, dstElem});
					break;
				default: break;
			}
		}

		// TODO: change/check for descriptor indexing
		dst.invalidateCbs();
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

	for(auto& entry : dut.entries) {
		auto dstBinding = entry.dstBinding;
		auto dstElem = entry.dstArrayElement;
		for(auto j = 0u; j < entry.descriptorCount; ++j, ++dstElem) {
			dlg_assert(dstBinding < ds.layout->bindings.size());
			while(dstElem >= ds.bindings[dstBinding].size()) {
				++dstBinding;
				dstElem = 0u;
				dlg_assert(dstBinding < ds.layout->bindings.size());
			}

			auto& binding = ds.bindings[dstBinding][dstElem];
			auto dsType = ds.layout->bindings[dstBinding].descriptorType;

			// NOTE: such an assertion here would be nice. Track used
			// layout in update?
			// dlg_assert(write.descriptorType == type);

			auto* data = ptr + (entry.offset + j * entry.stride);

			std::lock_guard lock(dev.mutex);
			switch(category(dsType)) {
				case DescriptorCategory::image: {
					auto& img = *reinterpret_cast<const VkDescriptorImageInfo*>(data);
					updateLocked(ds, binding, dstBinding, dstElem, img);
					break;
				} case DescriptorCategory::buffer: {
					auto& buf = *reinterpret_cast<const VkDescriptorBufferInfo*>(data);
					updateLocked(ds, binding, dstBinding, dstElem, buf);
					break;
				} case DescriptorCategory::bufferView: {
					auto& bufView = *reinterpret_cast<const VkBufferView*>(data);
					updateLocked(ds, binding, dstBinding, dstElem, bufView);
					break;
				} default:
					dlg_error("Invalid/unknown descriptor type");
					break;
			}
		}
	}

	ds.invalidateCbs();

	auto f = dev.dispatch.UpdateDescriptorSetWithTemplate;
	f(device, descriptorSet, descriptorUpdateTemplate, pData);
}

u32 totalUpdateDataSize(const DescriptorUpdateTemplate& dut) {
	u32 ret = 0u;
	for(auto& entry : dut.entries) {
		auto off = entry.offset + entry.descriptorCount * entry.stride;
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
