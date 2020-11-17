#include "device.hpp"
#include "layer.hpp"
#include "data.hpp"
#include "util.hpp"
#include "gui.hpp"
#include "swapchain.hpp"
#include "window.hpp"
#include "cb.hpp"
#include "rp.hpp"
#include "commands.hpp"
#include "memory.hpp"
#include "image.hpp"
#include "buffer.hpp"
#include "pipe.hpp"
#include "shader.hpp"
#include "ds.hpp"
#include "sync.hpp"

namespace fuen {

// util
Renderer* getWindowRenderer(Device& dev) {
	if(dev.window) {
		return &dev.window->renderer;
	}

	return nullptr;
}

Renderer* getOverlayRenderer(Swapchain& swapchain) {
	if(swapchain.overlay) {
		return &swapchain.overlay->renderer;
	}

	return nullptr;
}

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
		VkPhysicalDevice phdev,
		const VkDeviceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkDevice* dev) {
	auto* iniData = findData<Instance>(phdev);
	dlg_assert(iniData);

	auto* linkInfo = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(linkInfo && linkInfo->function != VK_LAYER_LINK_INFO) {
		linkInfo = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*linkInfo);
	}

	if(!linkInfo) {
		dlg_error("No linkInfo");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto fpGetInstanceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpGetDeviceProcAddr = linkInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(iniData->handle, "vkCreateDevice");
	if(!fpCreateDevice) {
		dlg_error("could not load vkCreateDevice");
		return VK_ERROR_INITIALIZATION_FAILED;
	}

  	// Advance the link info for the next element on the chain
	auto mutLinkInfo = const_cast<VkLayerDeviceCreateInfo*>(linkInfo);
   	mutLinkInfo->u.pLayerInfo = linkInfo->u.pLayerInfo->pNext;

	VkResult result = fpCreateDevice(phdev, ci, alloc, dev);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& devData = createData<Device>(*dev);
	devData.ini = iniData;
	devData.phdev = phdev;
	devData.handle = *dev;
	devData.dispatch = iniData->dispatch;
	devData.dispatch.vkGetInstanceProcAddr = fpGetInstanceProcAddr;
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;
	devData.dispatch.init((vk::Device) *dev);
	// In case GetDeviceProcAddr of the next chain didn't return itself
	devData.dispatch.vkGetDeviceProcAddr = fpGetDeviceProcAddr;

	devData.swapchains.mutex = &devData.mutex;
	devData.images.mutex = &devData.mutex;
	devData.imageViews.mutex = &devData.mutex;
	devData.buffers.mutex = &devData.mutex;
	devData.framebuffers.mutex = &devData.mutex;
	devData.renderPasses.mutex = &devData.mutex;
	devData.commandBuffers.mutex = &devData.mutex;
	devData.commandPools.mutex = &devData.mutex;
	devData.fences.mutex = &devData.mutex;
	devData.dsPools.mutex = &devData.mutex;
	devData.dsLayouts.mutex = &devData.mutex;
	devData.descriptorSets.mutex = &devData.mutex;
	devData.buffers.mutex = &devData.mutex;
	devData.deviceMemories.mutex = &devData.mutex;
	devData.shaderModules.mutex = &devData.mutex;
	devData.samplers.mutex = &devData.mutex;
	devData.computePipes.mutex = &devData.mutex;
	devData.graphicsPipes.mutex = &devData.mutex;
	devData.pipeLayouts.mutex = &devData.mutex;
	devData.events.mutex = &devData.mutex;
	devData.semaphores.mutex = &devData.mutex;

	// find vkSetDeviceLoaderData callback
	auto* loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*ci);
	while(loaderData && loaderData->function != VK_LOADER_DATA_CALLBACK) {
		loaderData = findChainInfo<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(*loaderData);
	}

	dlg_assert(loaderData);
	devData.setDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

   	// query queues
	u32 nqf;
	devData.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, nullptr);
	auto qfprops = std::make_unique<VkQueueFamilyProperties[]>(nqf);
	devData.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(phdev, &nqf, qfprops.get());

	// reserve vector so we don't get a reallocation
	auto nQueues = 0u;
	for(auto i = 0u; i < ci->queueCreateInfoCount; ++i) {
		nQueues += ci->pQueueCreateInfos[i].queueCount;
	}

	devData.queues.reserve(nQueues);
	for(auto i = 0u; i < ci->queueCreateInfoCount; ++i) {
		auto& qi = ci->pQueueCreateInfos[i];
		auto& familyProps = qfprops[qi.queueFamilyIndex];

		for(auto j = 0u; j < qi.queueCount; ++j) {
			auto& q = *devData.queues.emplace_back(std::make_unique<Queue>());
			q.dev = &devData;
			q.flags = familyProps.queueFlags;
			devData.dispatch.vkGetDeviceQueue(*dev, qi.queueFamilyIndex, j, &q.queue);

			// Queue is a dispatchable handle.
			// We therefore have to inform the loader that we created this
			// resource inside the layer and let it set its dispatch table.
			// We will also have to get our queue-data just from the VkQueue
			// later on (e.g. vkQueueSubmit) so associate data with it.
			devData.setDeviceLoaderData(*dev, q.queue);
			insertData(q.queue, &q);

			if(!devData.gfxQueue && familyProps.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				devData.gfxQueue = &q;
			}
		}
	}

	// query memory stuff
	VkPhysicalDeviceMemoryProperties memProps;
	devData.dispatch.vkGetPhysicalDeviceMemoryProperties(phdev, &memProps);
	for(auto i = 0u; i < memProps.memoryTypeCount; ++i) {
		auto flags = memProps.memoryTypes[i].propertyFlags;
		if(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
			devData.hostVisibleMemTypeBits |= (1 << i);
		}

		if(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
			devData.deviceLocalMemTypeBits |= (1 << i);
		}
	}

	if(devData.gfxQueue) {
		// sampler
		VkSamplerCreateInfo sci = vk::SamplerCreateInfo();
		// TODO: create multiple samplers, one with VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sci.magFilter = VK_FILTER_NEAREST;
		sci.minFilter = VK_FILTER_NEAREST;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.minLod = -1000;
		sci.maxLod = 1000;
		sci.maxAnisotropy = 1.0f;
		VK_CHECK(devData.dispatch.vkCreateSampler(*dev, &sci, nullptr, &devData.sampler));

		// command pool
		VkCommandPoolCreateInfo cpci = vk::CommandPoolCreateInfo();
		cpci.queueFamilyIndex = devData.gfxQueue->family;
		cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VK_CHECK(devData.dispatch.vkCreateCommandPool(*dev, &cpci, nullptr, &devData.commandPool));

		// descriptor set layout
		VkDescriptorSetLayoutBinding binding;
		binding.binding = 0u;
		binding.descriptorCount = 1u;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.pImmutableSamplers = &devData.sampler;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo dslci = vk::DescriptorSetLayoutCreateInfo();
		dslci.bindingCount = 1u;
		dslci.pBindings = &binding;
		VK_CHECK(devData.dispatch.vkCreateDescriptorSetLayout(*dev, &dslci, nullptr, &devData.dsLayout));

		// pipeline layout
		VkPushConstantRange pcrs[1] = {};
		pcrs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pcrs[0].offset = sizeof(float) * 0;
		pcrs[0].size = sizeof(float) * 4;

		VkPipelineLayoutCreateInfo plci = vk::PipelineLayoutCreateInfo();
		plci.setLayoutCount = 1;
		plci.pSetLayouts = &devData.dsLayout;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = pcrs;
		VK_CHECK(devData.dispatch.vkCreatePipelineLayout(*dev, &plci, nullptr, &devData.pipeLayout));

		// descriptoer pool
		VkDescriptorPoolSize poolSize;
		poolSize.descriptorCount = 50u;
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		VkDescriptorPoolCreateInfo dpci = vk::DescriptorPoolCreateInfo();
		dpci.pPoolSizes = &poolSize;
		dpci.poolSizeCount = 1u;
		dpci.maxSets = 50u;
		dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		VK_CHECK(devData.dispatch.vkCreateDescriptorPool(*dev, &dpci, nullptr, &devData.dsPool));
	}

	constexpr auto useWindow = true;
	if(useWindow) {
		devData.window = std::make_unique<DisplayWindow>();
		devData.window->init(devData);
	}

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
		VkDevice dev,
		VkAllocationCallbacks* alloc) {
	auto devd = moveData<Device>(dev);
	dlg_assert(devd);

	// user must have erased all resources
	dlg_assert(devd->swapchains.empty());
	dlg_assert(devd->images.empty());
	dlg_assert(devd->buffers.empty());
	dlg_assert(devd->imageViews.empty());
	dlg_assert(devd->framebuffers.empty());
	dlg_assert(devd->renderPasses.empty());
	dlg_assert(devd->commandPools.empty());
	dlg_assert(devd->commandBuffers.empty());
	dlg_assert(devd->fences.empty());
	dlg_assert(devd->dsPools.empty());
	dlg_assert(devd->dsLayouts.empty());
	dlg_assert(devd->descriptorSets.empty());
	dlg_assert(devd->deviceMemories.empty());
	dlg_assert(devd->shaderModules.empty());
	dlg_assert(devd->buffers.empty());

	// erase queue datas
	for(auto& queue : devd->queues) {
		eraseData(queue->queue);
	}

	// destroy our logical device before we call the function.
	auto* destroyDev = devd->dispatch.vkDestroyDevice;
	devd.release();
	destroyDev(dev, alloc);
}

VkFence getFenceFromPool(Device& dev) {
	{
		std::lock_guard lock(dev.mutex);
		if(!dev.fencePool.empty()) {
			auto ret = dev.fencePool.back();
			dev.fencePool.pop_back();
			return ret;
		}

		// check if a submission finished
		for(auto it = dev.pending.begin(); it < dev.pending.end(); ++it) {
			auto& subm = *it;
			if(subm->ourFence && checkLocked(*subm)) {
				dlg_assert(!dev.fencePool.empty());
				auto ret = dev.fencePool.back();
				dev.fencePool.pop_back();
				return ret;
			}
		}
	}

	// create new fence
	VkFence fence;
	VkFenceCreateInfo fci = vk::FenceCreateInfo();
	VK_CHECK(dev.dispatch.vkCreateFence(dev.handle, &fci, nullptr, &fence));
	return fence;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence) {
	auto& qd = getData<Queue>(queue);
	auto& dev = *qd.dev;

	// hook fence
	auto submPtr = std::make_unique<PendingSubmission>();
	auto& subm = *submPtr;
	subm.queue = &qd;

	for(auto i = 0u; i < submitCount; ++i) {
		auto& si = pSubmits[i];
		auto& dst = subm.submissions.emplace_back();

		for(auto j = 0u; j < si.signalSemaphoreCount; ++j) {
			dst.signalSemaphore.push_back(si.pSignalSemaphores[j]);
		}

		for(auto j = 0u; j < si.commandBufferCount; ++j) {
			auto& cb = dev.commandBuffers.get(si.pCommandBuffers[j]);
			dst.cbs.push_back(&cb);

			{
				// store in command buffer that it was submitted here
				std::lock_guard lock(dev.mutex);
				cb.pending.push_back(&subm);

				// store pending layouts
				for(auto& used : cb.images) {
					if(used.second.layoutChanged) {
						dlg_assert(
							used.second.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
							used.second.finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED);

						used.second.image->pendingLayout = used.second.finalLayout;
					}
				}
			}
		}

		for(auto j = 0u; j < si.waitSemaphoreCount; ++j) {
			dst.waitSemaphores.emplace_back(
				si.pWaitSemaphores[j],
				si.pWaitDstStageMask[j]);
		}
	}

	VkFence submFence;
	if(fence) {
		subm.appFence = &dev.fences.get(fence);

		std::lock_guard lock(dev.mutex);

		// per vulkan spec, using a fence in QueueSubmit that is signals
		// is not allowed. And if it was reset we also reset its associated
		// submission.
		dlg_assert(!subm.appFence->submission);
		submFence = fence;
		subm.appFence->submission = &subm;
	} else {
		subm.ourFence = getFenceFromPool(dev);
		submFence = subm.ourFence;
	}

	// Lock order here important, see mutex usage for rendering in swa.cpp.
	// Also important that we don't lock both mutexes at once.
	std::lock_guard queueLock(dev.queueMutex);

	{
		std::lock_guard lock(dev.mutex);
		dev.pending.push_back(std::move(submPtr));
	}

	return dev.dispatch.vkQueueSubmit(queue, submitCount, pSubmits, submFence);
}

bool checkLocked(PendingSubmission& subm) {
	auto& dev = *subm.queue->dev;

	if(subm.appFence) {
		std::lock_guard fenceLock(subm.appFence->mutex);
		if(dev.dispatch.vkGetFenceStatus(dev.handle, subm.appFence->handle) != VK_SUCCESS) {
			return false;
		}
	} else {
		dlg_assert(subm.ourFence);
		std::lock_guard fenceLock(subm.ourMutex);
		if(dev.dispatch.vkGetFenceStatus(dev.handle, subm.ourFence) != VK_SUCCESS) {
			return false;
		}
	}
	// apparently unique_ptr == ptr comparision not supported in stdlibc++ yet?
	auto it = std::find_if(dev.pending.begin(), dev.pending.end(), [&](auto& ptr){
			return ptr.get() == &subm;
	});
	dlg_assert(it != dev.pending.end());

	for(auto& sub : subm.submissions) {
		for(auto* cb : sub.cbs) {
			auto it2 = std::find(cb->pending.begin(), cb->pending.end(), &subm);
			dlg_assert(it2 != cb->pending.end());
			cb->pending.erase(it2);
		}
	}

	if(subm.ourFence) {
		dev.dispatch.vkResetFences(dev.handle, 1, &subm.ourFence);
		dev.fencePool.push_back(subm.ourFence);
	} else if(subm.appFence) {
		subm.appFence->submission = nullptr;
	}

	dev.pending.erase(it);
	return true;
}

void notifyDestruction(Device& dev, Handle& handle) {
	forEachRendererLocked(dev, [&](auto& renderer) {
		renderer.unselect(handle);
	});
}

} // namespace fuen
