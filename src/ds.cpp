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

			// If the given handle is valid (i.e. not a vulkan null handle),
			// retrieves the assocated object from the given map and adds
			// this ds to the list of reference in it.
			auto nullOrGet = [&](auto& map, auto& handle) -> decltype(map.getWeakPtr(handle)) {
				if(!handle) {
					return {};
				}

				auto it = map.map.find(handle);
				dlg_assert(it != map.map.end());
				return std::weak_ptr(it->second);
			};

			std::lock_guard lock(dev.mutex);

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image: {
					dlg_assert(write.pImageInfo);
					DescriptorSet::ImageInfo info {};
					info.layout = write.pImageInfo[j].imageLayout;
					info.imageView = nullOrGet(dev.imageViews, write.pImageInfo[j].imageView);
					info.sampler = nullOrGet(dev.samplers, write.pImageInfo[j].sampler);
					binding.data = info;
					break;
				} case DescriptorCategory::buffer: {
					dlg_assert(write.pBufferInfo);
					DescriptorSet::BufferInfo info {};
					info.buffer = dev.buffers.getWeakPtrLocked(write.pBufferInfo[j].buffer);
					info.offset = write.pBufferInfo[j].offset;
					info.range = write.pBufferInfo[j].range;
					binding.data = info;
					break;
				} case DescriptorCategory::bufferView:
					dlg_assert(write.pTexelBufferView);
					binding.data = dev.bufferViews.getWeakPtrLocked(write.pTexelBufferView[j]);
					break;
				default: break;
			}

			binding.valid = true;
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

} // namespace fuen
