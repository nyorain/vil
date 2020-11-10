#include "ds.hpp"
#include "data.hpp"

namespace fuen {

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
	auto res = dev.dispatch.vkCreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dsLayout = dev.dsLayouts.add(*pSetLayout);
	dsLayout.dev = &dev;
	dsLayout.handle = *pSetLayout;

	for(auto i = 0u; i < pCreateInfo->bindingCount; ++i) {
		dsLayout.bindings.push_back(pCreateInfo->pBindings[i]);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
		VkDevice                                    device,
		VkDescriptorSetLayout                       descriptorSetLayout,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.dsLayouts.mustErase(descriptorSetLayout);
	dev.dispatch.vkDestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator);
}

// dsPool
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(
		VkDevice                                    device,
		const VkDescriptorPoolCreateInfo*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDescriptorPool*                           pDescriptorPool) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dsPool = dev.dsPools.add(*pDescriptorPool);
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
	dev.dispatch.vkDestroyDescriptorPool(device, descriptorPool, pAllocator);
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
	return dev.dispatch.vkResetDescriptorPool(device, descriptorPool, flags);
}

// descriptor set
VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(
		VkDevice                                    device,
		const VkDescriptorSetAllocateInfo*          pAllocateInfo,
		VkDescriptorSet*                            pDescriptorSets) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& pool = dev.dsPools.get(pAllocateInfo->descriptorPool);
	for(auto i = 0u; i < pAllocateInfo->descriptorSetCount; ++i) {
		auto& ds = dev.descriptorSets.add(pDescriptorSets[i]);
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
		auto ds = dev.descriptorSets.mustMove(pDescriptorSets[i]);
		auto it = std::find(ds->pool->descriptorSets.begin(), ds->pool->descriptorSets.end(),
			ds.get());
		dlg_assert(it != ds->pool->descriptorSets.end());
		ds->pool->descriptorSets.erase(it);
	}

	return dev.dispatch.vkFreeDescriptorSets(device, descriptorPool,
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
			binding.valid = true;

			switch(category(write.descriptorType)) {
				case DescriptorCategory::image:
					dlg_assert(write.pImageInfo);
					binding.imageInfo = write.pImageInfo[j];
					break;
				case DescriptorCategory::buffer:
					dlg_assert(write.pBufferInfo);
					binding.bufferInfo = write.pBufferInfo[j];
					break;
				case DescriptorCategory::bufferView:
					dlg_assert(write.pTexelBufferView);
					binding.bufferView = write.pTexelBufferView[j];
					break;
				default: break;
			}
		}
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

			dst.bindings[dstBinding][dstElem] = src.bindings[srcBinding][srcElem];
		}
	}

	return dev.dispatch.vkUpdateDescriptorSets(device,
		descriptorWriteCount, pDescriptorWrites,
		descriptorCopyCount, pDescriptorCopies);
}

} // namespace fuen
