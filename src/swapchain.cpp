#include <swapchain.hpp>
#include <data.hpp>
#include <record.hpp>
#include <layer.hpp>
#include <image.hpp>
#include <queue.hpp>
#include <platform.hpp>
#include <overlay.hpp>
#include <vk/enumString.hpp>

namespace vil {

Swapchain::~Swapchain() {
	overlay.reset();
	destroy();
}

void Swapchain::destroy() {
	if(!dev) {
		return;
	}

	if(dev->swapchain == this) {
		dev->swapchain = nullptr;
	}

	for(auto* img : this->images) {
		dev->images.mustErase(img->handle);
	}

	images.clear();

	// TODO: not sure about this. We don't synchronize access to it
	// in other places since the api for swapchain/overlay retrieval
	// can't be threadsafe, by design.
	std::lock_guard lock(dev->mutex);
	if(dev->lastCreatedSwapchain == this) {
		dev->lastCreatedSwapchain = nullptr;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
		VkDevice                                    device,
		const VkSwapchainCreateInfoKHR*             pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSwapchainKHR*                             pSwapchain) {
	auto& dev = getData<Device>(device);
	auto* platform = findData<Platform>(pCreateInfo->surface);

	// It's important we *first* destroy our image objects coming
	// from this swapchain since they may implicitly destroyed
	// inside the CreateSwapchain call.
	// Even when the new CreateSwapchain call fails, the images might
	// be destroyed via vulkan spec and the old swapchain becomes
	// unusable.
	Swapchain* oldChain {};
	std::unique_ptr<Overlay> savedOverlay {};
	bool recreateOverlay {false};
	if(pCreateInfo->oldSwapchain) {
		oldChain = &dev.swapchains.get(pCreateInfo->oldSwapchain);

		// This is important to destroy our handles of the swapchain
		// images.
		oldChain->destroy();

		if(oldChain->overlay) {
			if(Overlay::compatible(oldChain->ci, *pCreateInfo)) {
				// Have to make sure previous rendering has finished.
				// We can be sure gui isn't starting new draws in another
				// thread.
				oldChain->overlay->gui.waitForDraws();
				savedOverlay = std::move(oldChain->overlay);
			} else {
				recreateOverlay = true;
			}
		}
	}

	// check surface capabilities to see if we can sneak in transferSrc
	// for our command hook copies
	auto sci = *pCreateInfo;

	VkSurfaceCapabilitiesKHR caps;
	auto res = dev.ini->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phdev,
		pCreateInfo->surface, &caps);
	if(res != VK_SUCCESS) {
		dlg_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR: {} ({})", vk::name(res), res);
	} else if(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
		sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	auto result = dev.dispatch.CreateSwapchainKHR(device, &sci, pAllocator, pSwapchain);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& swapd = dev.swapchains.add(*pSwapchain);
	swapd.objectType = VK_OBJECT_TYPE_SWAPCHAIN_KHR;
	swapd.dev = &dev;
	swapd.ci = *pCreateInfo;
	swapd.handle = *pSwapchain;

	// add swapchain images to tracked images
	u32 imgCount = 0u;
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapd.handle, &imgCount, nullptr));
	auto imgs = std::make_unique<VkImage[]>(imgCount);
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapd.handle, &imgCount, imgs.get()));

	swapd.images.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		auto& img = dev.images.add(imgs[i]);
		img.swapchain = &swapd;
		img.swapchainImageID = i;
		img.dev = &dev;
		img.objectType = VK_OBJECT_TYPE_IMAGE;
		img.handle = imgs[i];
		img.hasTransferSrc = (sci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

		img.ci.arrayLayers = sci.imageArrayLayers;
		img.ci.imageType = VK_IMAGE_TYPE_2D;
		img.ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		img.ci.extent = {sci.imageExtent.width, sci.imageExtent.height, 1u};
		img.ci.format = sci.imageFormat;
		img.ci.mipLevels = 1u;
		img.ci.samples = VK_SAMPLE_COUNT_1_BIT;
		img.ci.sharingMode = sci.imageSharingMode;
		img.ci.usage = sci.imageUsage;
		img.ci.tiling = VK_IMAGE_TILING_OPTIMAL; // don't really know
		// TODO: make local copy and store them here
		// img.ci.pQueueFamilyIndices = sci.pQueueFamilyIndices;
		// img.ci.queueFamilyIndexCount = sci.queueFamilyIndexCount;

		swapd.images[i] = &img;
	}

	dev.lastCreatedSwapchain = &swapd;

	if(savedOverlay) {
		swapd.overlay = std::move(savedOverlay);
		swapd.overlay->swapchain = &swapd;

		{
			std::lock_guard lock(dev.mutex);
			swapd.overlay->initRenderBuffers();
		}

		if(swapd.overlay->platform) {
			swapd.overlay->platform->resize(swapd.ci.imageExtent.width, swapd.ci.imageExtent.height);
		}
	} else if(recreateOverlay) {
		// Otherwise we have to create a new renderer from scratch.
		// Carry over all gui logic. Just recreate rendering logic
		dlg_error("TODO: not implemented");
	} else if(platform) {
		swapd.overlay = std::make_unique<Overlay>();
		swapd.overlay->init(swapd);

		swapd.overlay->platform = platform;
		platform->init(*swapd.dev, swapd.ci.imageExtent.width, swapd.ci.imageExtent.height);
	}

	// TODO: hacky, see cb gui
	dev.swapchain = &swapd;

	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
		VkDevice                                    device,
		VkSwapchainKHR                             	swapchain,
		const VkAllocationCallbacks*                pAllocator) {
	if(!swapchain) {
		return;
	}

	auto& devd = getData<Device>(device);
	devd.swapchains.mustErase(swapchain);
	devd.dispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
		VkQueue                                     queue,
		const VkPresentInfoKHR*                     pPresentInfo) {
	auto& qd = getData<Queue>(queue);

	auto combinedResult = VK_SUCCESS;
	for(auto i = 0u; i < pPresentInfo->swapchainCount; ++i) {
		auto& swapchain = qd.dev->swapchains.get(pPresentInfo->pSwapchains[i]);
		VkResult res;

		if(swapchain.overlay && swapchain.overlay->platform) {
			swapchain.overlay->show = swapchain.overlay->platform->update(swapchain.overlay->gui);
		}

		++swapchain.presentCounter;
		std::swap(swapchain.frameSubmissions, swapchain.nextFrameSubmissions);
		swapchain.nextFrameSubmissions.clear();

		auto now = Swapchain::Clock::now();
		if(swapchain.lastPresent) {
			if(swapchain.frameTimings.size() == swapchain.maxFrameTimings) {
				swapchain.frameTimings.erase(swapchain.frameTimings.begin());
			}

			auto timing = now - *swapchain.lastPresent;
			swapchain.frameTimings.push_back(timing);
		}
		swapchain.lastPresent = now;

		if(swapchain.overlay && swapchain.overlay->show) {
			auto waitsems = span{pPresentInfo->pWaitSemaphores, pPresentInfo->waitSemaphoreCount};
			res = swapchain.overlay->drawPresent(qd, waitsems, pPresentInfo->pImageIndices[i]);
		} else {
			VkPresentInfoKHR pi {};
			pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			pi.pImageIndices = &pPresentInfo->pImageIndices[i];
			pi.pResults = pPresentInfo->pResults ? &pPresentInfo->pResults[i] : nullptr;
			pi.pSwapchains = &pPresentInfo->pSwapchains[i];
			pi.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
			pi.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
			pi.swapchainCount = 1u;
			// TODO: might be bad to not forward this
			// pi.pNext

			std::lock_guard queueLock(qd.dev->queueMutex);
			res = qd.dev->dispatch.QueuePresentKHR(queue, &pi);
		}

		if(pPresentInfo->pResults) {
			pPresentInfo->pResults[i] = res;
		}

		if(res != VK_SUCCESS && combinedResult == VK_SUCCESS) {
			combinedResult = res;
		}
	}

	return combinedResult;
}

} // namespace vil
