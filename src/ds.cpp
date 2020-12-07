#include "ds.hpp"
#include "data.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "util.hpp"

namespace fuen {

// Classes
void unregisterLocked(DescriptorSet& ds, unsigned binding, unsigned elem) {
	dlg_assert(ds.dev);
	dlg_assert(ds.bindings.size() > binding);
	dlg_assert(ds.bindings[binding].size() > elem);

	auto& bind = ds.bindings[binding][elem];
	dlg_assert(bind.valid);

	auto removeFromHandle = [&](auto& handle) {
		auto& dsrefs = handle.descriptors;
		auto finder = [&](const DescriptorSetRef& dsref) {
			return dsref.ds == &ds && dsref.binding == binding && dsref.elem == elem;
		};
		auto it = std::find_if(dsrefs.begin(), dsrefs.end(), finder);
		dlg_assert(it != dsrefs.end());
		dsrefs.erase(it);
	};

	switch(category(ds.layout->bindings[binding].descriptorType)) {
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
		} default: dlg_error("Unimplemented descriptor type"); break;
	}
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

void DescriptorSet::invalidateLocked(unsigned binding, unsigned elem) {
	unregisterLocked(*this, binding, elem);
	this->bindings[binding][elem] = {};

	// TODO: change/check for descriptor indexing
	this->invalidateCbsLocked();
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
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dsLayout = dev.dsLayouts.add(*pSetLayout);
	dsLayout.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
	dsLayout.dev = &dev;
	dsLayout.handle = *pSetLayout;

	for(auto i = 0u; i < pCreateInfo->bindingCount; ++i) {
		dsLayout.bindings.push_back(pCreateInfo->pBindings[i]);
	}

	std::sort(dsLayout.bindings.begin(), dsLayout.bindings.end(),
		[](auto& ba, auto& bb) { return ba.binding < bb.binding; });

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
		VkDevice                                    device,
		VkDescriptorSetLayout                       descriptorSetLayout,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dsLayouts.mustErase(descriptorSetLayout);
	dev.dispatch.DestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator);
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

	for(auto& ds : dsPool.descriptorSets) {
		dev.descriptorSets.mustErase(ds->handle);
	}

	dsPool.descriptorSets.clear();
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
		ds.layout = &dev.dsLayouts.get(pAllocateInfo->pSetLayouts[i]);
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

			std::lock_guard lock(dev.mutex);
			binding.valid = true;

			// Adds this ds to the list of references in the given resource handle.
			auto addDsRef = [&](auto* res) {
				if(!res) {
					return;
				}

				res->descriptors.push_back({&ds, dstBinding, dstElem});
			};

			// If the given handle is valid (i.e. not a vulkan null handle),
			// retrieves the assocated object from the given map and adds
			// this ds to the list of reference in it.
			auto nullOrGetAdd = [&](auto& map, auto& handle) -> decltype(&map.get(handle)) {
				if(!handle) {
					return nullptr;
				}

				auto& res = map.get(handle);
				res.descriptors.push_back({&ds, dstBinding, dstElem});
				return &res;
			};

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image:
					dlg_assert(write.pImageInfo);
					binding.imageInfo.layout = write.pImageInfo[j].imageLayout;
					binding.imageInfo.imageView = nullOrGetAdd(dev.imageViews, write.pImageInfo[j].imageView);
					binding.imageInfo.sampler = nullOrGetAdd(dev.samplers, write.pImageInfo[j].sampler);
					break;
				case DescriptorCategory::buffer:
					dlg_assert(write.pBufferInfo);
					binding.bufferInfo.buffer = &dev.buffers.get(write.pBufferInfo[j].buffer);
					addDsRef(binding.bufferInfo.buffer);
					binding.bufferInfo.offset = write.pBufferInfo[j].offset;
					binding.bufferInfo.range = write.pBufferInfo[j].range;
					break;
				case DescriptorCategory::bufferView:
					// TODO
					dlg_error("Buffer views unimplemented");
					/*
					dlg_assert(write.pTexelBufferView);
					binding.bufferView = write.pTexelBufferView[j];
					*/
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

} // namespace fuen
