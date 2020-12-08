#include "handle.hpp"
#include "data.hpp"
#include "device.hpp"

#include "swapchain.hpp"
#include "cb.hpp"
#include "memory.hpp"
#include "image.hpp"
#include "buffer.hpp"
#include "rp.hpp"
#include "pipe.hpp"
#include "shader.hpp"
#include "sync.hpp"
#include "ds.hpp"

namespace fuen {

DeviceHandle::~DeviceHandle() {
	if(!dev) {
		return;
	}

	// Inform command buffers that use this handle that it was
	// destroyed.
	invalidateCbs();

	// Notify device that handle is destroyed. It will forward it
	// to all instances that need to know (e.g. gui)
	notifyDestruction(*dev, *this);
}

void DeviceHandle::invalidateCbsLocked() {
	while(!refCbs.empty()) {
		auto* first = refCbs[0];
		first->invalidateLocked();
	}
}

void DeviceHandle::invalidateCbs() {
	// We have to lock the mutex here since other threads might access
	// this->refCbs (e.g. other command buffers being destroyed and removing
	// themselves).
	dlg_assert(this->dev);
	std::lock_guard lock(this->dev->mutex);
	invalidateCbsLocked();
}

Handle* findHandle(Device& dev, VkObjectType objectType, u64 handle) {
	switch(objectType) {
		case VK_OBJECT_TYPE_QUEUE:
			// queues is dispatchable
			return findData<Queue>((VkQueue) handle);
		case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
			return dev.swapchains.find((VkSwapchainKHR) handle);
		case VK_OBJECT_TYPE_IMAGE:
		  	return dev.images.find((VkImage) handle);
		case VK_OBJECT_TYPE_IMAGE_VIEW:
		  	return dev.imageViews.find((VkImageView) handle);
		case VK_OBJECT_TYPE_BUFFER:
			return dev.buffers.find((VkBuffer) handle);
		case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
			return dev.dsPools.find((VkDescriptorPool) handle);
		case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
			return dev.dsLayouts.find((VkDescriptorSetLayout) handle);
		case VK_OBJECT_TYPE_DESCRIPTOR_SET:
			return dev.descriptorSets.find((VkDescriptorSet) handle);
		case VK_OBJECT_TYPE_SHADER_MODULE:
			return dev.shaderModules.find((VkShaderModule) handle);
		case VK_OBJECT_TYPE_SAMPLER:
			return dev.samplers.find((VkSampler) handle);
		case VK_OBJECT_TYPE_DEVICE_MEMORY:
			return dev.deviceMemories.find((VkDeviceMemory) handle);
		case VK_OBJECT_TYPE_PIPELINE: {
			auto gp = dev.graphicsPipes.find((VkPipeline) handle);
			if(gp) {
				return gp;
			}

			return dev.computePipes.find((VkPipeline) handle);
		} case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
			return dev.pipeLayouts.find((VkPipelineLayout) handle);
		case VK_OBJECT_TYPE_FENCE:
			return dev.fences.find((VkFence) handle);
		case VK_OBJECT_TYPE_EVENT:
			return dev.events.find((VkEvent) handle);
		case VK_OBJECT_TYPE_SEMAPHORE:
			return dev.semaphores.find((VkSemaphore) handle);
		case VK_OBJECT_TYPE_FRAMEBUFFER:
			return dev.framebuffers.find((VkFramebuffer) handle);
		case VK_OBJECT_TYPE_RENDER_PASS:
			return dev.renderPasses.find((VkRenderPass) handle);
		case VK_OBJECT_TYPE_COMMAND_POOL:
			return dev.commandPools.find((VkCommandPool) handle);
		case VK_OBJECT_TYPE_COMMAND_BUFFER:
			return dev.commandBuffers.find((VkCommandBuffer) handle);
		default: break;
	}

	return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
		VkDevice                                    device,
		const VkDebugUtilsObjectNameInfoEXT*        pNameInfo) {
	auto& devd = getData<Device>(device);
	auto* handle = findHandle(devd, pNameInfo->objectType, pNameInfo->objectHandle);
	if(handle) {
		std::lock_guard lock(devd.mutex);
		handle->name = pNameInfo->pObjectName;
	}

	if(!devd.dispatch.SetDebugUtilsObjectNameEXT) {
		return VK_SUCCESS;
	}

	return devd.dispatch.SetDebugUtilsObjectNameEXT(device, pNameInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
		VkDevice                                    device,
		const VkDebugUtilsObjectTagInfoEXT*         pTagInfo) {
	auto& devd = getData<Device>(device);
	auto* handle = findHandle(devd, pTagInfo->objectType, pTagInfo->objectHandle);
	if(handle) {
		std::lock_guard lock(devd.mutex);
		auto& data = handle->tags[pTagInfo->tagName];
		auto ptr = reinterpret_cast<const std::byte*>(pTagInfo->pTag);
		data = {ptr, ptr + pTagInfo->tagSize};
	}

	if(!devd.dispatch.SetDebugUtilsObjectTagEXT) {
		return VK_SUCCESS;
	}

	return devd.dispatch.SetDebugUtilsObjectTagEXT(device, pTagInfo);
}

} // namespace fuen
