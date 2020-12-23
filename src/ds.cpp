#include "ds.hpp"
#include "data.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "util.hpp"

namespace fuen {

// Classes
DescriptorSet::~DescriptorSet() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);
	// Remove from descriptor pool.
	// Pools can't be destroyed before their sets as they implicitly free them.
	dlg_assert(pool);

	auto it = find(pool->descriptorSets, this);
	dlg_assert(it != pool->descriptorSets.end());
	pool->descriptorSets.erase(it);
}

/*
std::weak_ptr<Sampler> DescriptorSet::getSampler(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(bindings[binding]. > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::image);
	return bindings[binding][elem].imageInfo.sampler;
}

std::weak_ptr<ImageView> DescriptorSet::getImageView(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::image);
	return bindings[binding][elem].imageInfo.imageView;
}

std::weak_ptr<Buffer> DescriptorSet::getBuffer(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::buffer);
	return bindings[binding][elem].bufferInfo.buffer;
}

std::weak_ptr<BufferView> DescriptorSet::getBufferView(unsigned binding, unsigned elem) {
	dlg_assert(bindings.size() > binding);
	dlg_assert(bindings[binding].size() > elem);
	dlg_assert(category(this->layout->bindings[binding].descriptorType) == DescriptorCategory::bufferView);
	return bindings[binding][elem].bufferView;
}
*/

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
	dlg_assert(refCbs.empty());
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
		auto& bind = pCreateInfo->pBindings[i];
		ensureSize(dsLayout.bindings, bind.binding + 1);
		dsLayout.bindings[bind.binding] = bind;
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

void updateLocked(Device& dev, DescriptorSet::Binding& binding, VkBufferView bufferView) {
	dlg_assert(bufferView);
	binding.data = dev.bufferViews.getPtr(bufferView);
	binding.valid = true;
}

void updateLocked(Device& dev, DescriptorSet::Binding& binding, const VkDescriptorImageInfo& img) {
	DescriptorSet::ImageInfo info {};
	info.layout = img.imageLayout;
	if(img.imageView) {
		info.imageView = dev.imageViews.getWeakPtrLocked(img.imageView);
	}
	if(img.sampler) {
		info.sampler = dev.samplers.getWeakPtrLocked(img.sampler);
	}
	binding.data = info;
	binding.valid = true;
}

void updateLocked(Device& dev, DescriptorSet::Binding& binding, const VkDescriptorBufferInfo& buf) {
	DescriptorSet::BufferInfo info {};
	info.buffer = dev.buffers.getWeakPtrLocked(buf.buffer);
	info.offset = buf.offset;
	info.range = buf.range;
	binding.data = info;
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
			switch(category(write.descriptorType)) {
				case DescriptorCategory::image: {
					dlg_assert(write.pImageInfo);
					updateLocked(dev, binding, write.pImageInfo[j]);
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(write.pBufferInfo);
					updateLocked(dev, binding, write.pBufferInfo[j]);
					break;
				} case DescriptorCategory::bufferView:
					dlg_assert(write.pTexelBufferView);
					updateLocked(dev, binding, write.pTexelBufferView[j]);
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

			std::lock_guard lock(dev.mutex);
			dst.bindings[dstBinding][dstElem] = src.bindings[srcBinding][srcElem];
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
	auto res = dev.dispatch.CreateDescriptorUpdateTemplate(device, pCreateInfo,
		pAllocator, pDescriptorUpdateTemplate);
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
	dev.dispatch.DestroyDescriptorUpdateTemplate(device, descriptorUpdateTemplate, pAllocator);
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
					updateLocked(dev, binding, img);
					break;
				} case DescriptorCategory::buffer: {
					auto& buf = *reinterpret_cast<const VkDescriptorBufferInfo*>(data);
					updateLocked(dev, binding, buf);
					break;
				} case DescriptorCategory::bufferView: {
					auto& bufView = *reinterpret_cast<const VkBufferView*>(data);
					updateLocked(dev, binding, bufView);
					break;
				} default: break;
			}
		}
	}

	dev.dispatch.UpdateDescriptorSetWithTemplate(device, descriptorSet,
		descriptorUpdateTemplate, pData);
}

} // namespace fuen
