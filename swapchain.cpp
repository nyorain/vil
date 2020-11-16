#include "swapchain.hpp"
#include "data.hpp"
#include "image.hpp"
#include "gui.hpp"

namespace fuen {

Swapchain::~Swapchain() {
	// TODO
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
		VkDevice                                    device,
		const VkSwapchainCreateInfoKHR*             pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSwapchainKHR*                             pSwapchain) {
	auto& devd = getData<Device>(device);
	auto result = devd.dispatch.vkCreateSwapchainKHR(device, pCreateInfo,
		pAllocator, pSwapchain);

	if(result != VK_SUCCESS) {
		return result;
	}

	// TODO: try to simply reuse all of pCreateInfo->oldSwapchain.
	auto pswapd = std::make_unique<Swapchain>();
	auto& swapd = *pswapd;
	swapd.dev = &devd;
	swapd.ci = *pCreateInfo;
	swapd.swapchain = *pSwapchain;

	// add swapchain images to tracked images
	u32 imgCount = 0u;
	VK_CHECK(devd.dispatch.vkGetSwapchainImagesKHR(devd.handle, swapd.swapchain, &imgCount, nullptr));
	auto imgs = std::make_unique<VkImage[]>(imgCount);
	VK_CHECK(devd.dispatch.vkGetSwapchainImagesKHR(devd.handle, swapd.swapchain, &imgCount, imgs.get()));

	swapd.images.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		auto& img = devd.images.add(imgs[i]);
		img.swapchain = &swapd;
		img.swapchainImageID = i;
		img.dev = &devd;
		img.handle = imgs[i];

		swapd.images[i] = &img;
	}

	// overlay?
	constexpr auto overlay = false;
	if(overlay) {
		swapd.overlay = std::make_unique<Overlay>();
		swapd.overlay->init(swapd);
	}

	devd.swapchains.mustEmplace(*pSwapchain, std::move(pswapd));
	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
		VkDevice                                    device,
		VkSwapchainKHR                             	swapchain,
		const VkAllocationCallbacks*                pAllocator) {
	auto& devd = getData<Device>(device);

	auto& sc = devd.swapchains.get(swapchain);
	for(auto* img : sc.images) {
		devd.images.mustErase(img->handle);
	}

	devd.swapchains.mustErase(swapchain);

	devd.dispatch.vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
		VkQueue                                     queue,
		const VkPresentInfoKHR*                     pPresentInfo) {
	auto& qd = getData<Queue>(queue);

	auto combinedResult = VK_SUCCESS;
	for(auto i = 0u; i < pPresentInfo->swapchainCount; ++i) {
		auto& swapchain = qd.dev->swapchains.get(pPresentInfo->pSwapchains[i]);
		VkResult res;
		if(swapchain.overlay) {
			auto waitsems = span{pPresentInfo->pWaitSemaphores, pPresentInfo->waitSemaphoreCount};
			res = swapchain.overlay->drawPresent(qd, waitsems, pPresentInfo->pImageIndices[i]);
		} else {
			VkPresentInfoKHR pi = vk::PresentInfoKHR();
			pi.pImageIndices = &pPresentInfo->pImageIndices[i];
			pi.pResults = pPresentInfo->pResults ? &pPresentInfo->pResults[i] : nullptr;
			pi.pSwapchains = &pPresentInfo->pSwapchains[i];
			pi.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
			pi.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
			pi.swapchainCount = 1u;
			// TODO: might be bad to not forward this
			// pi.pNext

			std::lock_guard queueLock(qd.dev->queueMutex);
			res = qd.dev->dispatch.vkQueuePresentKHR(queue, &pi);
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

} // namespace fuen
