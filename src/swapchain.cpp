#include <swapchain.hpp>
#include <wrap.hpp>
#include <layer.hpp>
#include <threadContext.hpp>
#include <image.hpp>
#include <sync.hpp>
#include <queue.hpp>
#include <platform.hpp>
#include <overlay.hpp>
#include <command/record.hpp>
#include <util/profiling.hpp>
#include <vkutil/enumString.hpp>

namespace vil {

Swapchain::~Swapchain() {
	overlay.reset();
	destroy();
}

void Swapchain::destroy() {
	if(!dev) {
		return;
	}

	for(auto* img : this->images) {
		auto handle = castDispatch<VkImage>(*img);

		img->onApiDestroy();

		{
			std::lock_guard lock(dev->mutex);
			img->swapchain = nullptr;
			img->handle = {};

			dlg_assert(img->memory.index() == 0u);
			auto& memBind = std::get<0>(img->memory);
			memBind.memState = FullMemoryBind::State::resourceDestroyed;
		}

		// TODO: call onApiDestroy? shouldn't be needed atm but might
		// be in future. Not sure if expected, the api object
		// was always of implicit nature.
		dev->images.mustErase(handle);
	}

	// TODO: not sure about this. We don't synchronize access to it
	// in other places since the api for swapchain/overlay retrieval
	// can't be threadsafe, by design.
	std::lock_guard lock(dev->mutex);
	images.clear();
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
		VkDevice                                    device,
		const VkSwapchainCreateInfoKHR*             pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSwapchainKHR*                             pSwapchain) {
	auto& dev = getDevice(device);
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
		// NOTE: valid per spec to pass in swapchains created by
		// ANOTHER device. Don't handle this case rn but need to ignore
		// the parameter in that case, we can't assume it's a known
		// swapchain.
		oldChain = dev.swapchains.find(pCreateInfo->oldSwapchain);

		if(oldChain) {
			// This is important to destroy our handles of the swapchain
			// images.
			oldChain->destroy();

			if(oldChain->overlay) {
				if(Overlay::compatible(oldChain->ci, *pCreateInfo)) {
					// Have to make sure previous rendering has finished.
					// We can be sure gui isn't starting new draws in another
					// thread.
					oldChain->overlay->gui->waitForDraws();
					savedOverlay = std::move(oldChain->overlay);
				} else {
					recreateOverlay = true;
				}
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
	} else {
#ifndef VIL_NO_SWAPCHAIN_MOD
		// We always need color_attachment when drawing a gui.
		// Vulkan guarantees that it's supported.
		dlg_assert(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		sci.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

		if(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
			sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}

		if(caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
			sci.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
#endif // VIL_NO_SWAPCHAIN_MOD
	}

	auto result = dev.dispatch.CreateSwapchainKHR(device, &sci, pAllocator, pSwapchain);
	if(result != VK_SUCCESS) {
		return result;
	}

	auto& swapd = dev.swapchains.add(*pSwapchain);
	swapd.dev = &dev;
	swapd.ci = *pCreateInfo;
	swapd.handle = *pSwapchain;
	swapd.supportsSampling = (sci.imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT);

	// use data from old swapchain
	if(oldChain) {
		swapd.presentCounter = oldChain->presentCounter;
		swapd.lastPresent = std::move(oldChain->lastPresent);
		swapd.frameTimings = std::move(oldChain->frameTimings);
		swapd.frameSubmissions = std::move(oldChain->frameSubmissions);
		swapd.nextFrameSubmissions = std::move(oldChain->nextFrameSubmissions);
	}

	// Add swapchain images to tracked images
	u32 imgCount = 0u;
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapd.handle, &imgCount, nullptr));

	ThreadMemScope memScope;
	auto imgs = memScope.alloc<VkImage>(imgCount);
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapd.handle, &imgCount, imgs.data()));

	swapd.images.resize(imgCount);
	for(auto i = 0u; i < imgCount; ++i) {
		auto imgPtr = IntrusivePtr<Image>(new Image());
		auto& img = *imgPtr;
		img.swapchain = &swapd;
		img.swapchainImageID = i;
		img.dev = &dev;
		img.memObjectType = VK_OBJECT_TYPE_IMAGE;
		img.handle = imgs[i];
		img.hasTransferSrc = (sci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		img.allowsNearestSampling = (sci.imageUsage & VK_IMAGE_USAGE_SAMPLED_BIT);
		// TODO: special case, not sure how to represent it.
		// It's bound but implicitly, so we can't set img.memory
		// Might trigger asserts somewhere (that expect 'state == bound' -> 'memory != null')
		// std::get<0>(img.memory).memState = FullMemoryBind::State::bound;

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

		auto handleDown = castDispatch<VkImage>(img);
		dev.images.mustEmplace(handleDown, std::move(imgPtr));
	}

	dlg_trace(">> Createswapchain. platform: {}, dev.gui {}", platform, dev.gui());

	if(savedOverlay) {
		dlg_trace(">>savedOverlay");

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
		dlg_trace(">> creating overlay");

		swapd.overlay = std::make_unique<Overlay>();
		swapd.overlay->init(swapd);

		swapd.overlay->platform = platform;
		platform->init(*swapd.dev, swapd.ci.imageExtent.width, swapd.ci.imageExtent.height);
	}

	dev.swapchain(IntrusivePtr<Swapchain>(&swapd));
	return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(
		VkDevice                                    device,
		VkSwapchainKHR                             	swapchain,
		const VkAllocationCallbacks*                pAllocator) {
	if(!swapchain) {
		return;
	}

	auto swapchainPtr = mustMoveUnset(device, swapchain);
	auto& dev = *swapchainPtr->dev;
	dev.swapchainDestroyed(*swapchainPtr);
	swapchainPtr->destroy();

	dev.dispatch.DestroySwapchainKHR(dev.handle, swapchain, pAllocator);
}

void doAcquireImage(Swapchain& swapchain, VkSemaphore& vkSemaphore, VkFence& vkFence) {
	auto& dev = *swapchain.dev;

	if(vkSemaphore) {
		auto& semaphore = get(dev, vkSemaphore);
		dlg_assert(semaphore.type == VK_SEMAPHORE_TYPE_BINARY);
		dlg_assert(semaphore.signals.empty() ||
			semaphore.signals.back()->counterpart ||
			(semaphore.signals.back() == &SyncOp::swapchainAcquireDummy &&
				!semaphore.waits.empty() &&
				semaphore.waits.back()->counterpart == &SyncOp::swapchainAcquireDummy));

		semaphore.signals.push_back(&SyncOp::swapchainAcquireDummy);
		vkSemaphore = semaphore.handle;
	}

	if(vkFence) {
		auto& fence = get(dev, vkFence);
		vkFence = fence.handle;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2KHR(
		VkDevice                                    device,
		const VkAcquireNextImageInfoKHR*            pAcquireInfo,
		uint32_t*                                   pImageIndex) {
	auto& swapchain = get(device, pAcquireInfo->swapchain);
	auto& dev = *swapchain.dev;

	auto cpy = *pAcquireInfo;
	cpy.swapchain = swapchain.handle;

	doAcquireImage(swapchain, cpy.semaphore, cpy.fence);

	return dev.dispatch.AcquireNextImage2KHR(device, &cpy, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(
		VkDevice                                    device,
		VkSwapchainKHR                              vkSwapchain,
		uint64_t                                    timeout,
		VkSemaphore                                 vkSemaphore,
		VkFence                                     vkFence,
		uint32_t*                                   pImageIndex) {
	auto& swapchain = get(device, vkSwapchain);
	auto& dev = *swapchain.dev;

	doAcquireImage(swapchain, vkSemaphore, vkFence);

	return dev.dispatch.AcquireNextImageKHR(dev.handle, swapchain.handle,
		timeout, vkSemaphore, vkFence, pImageIndex);
}

void swapchainPresent(Swapchain& swapchain) {
	// update swapchain data
	FrameSubmissions keepAliveFrameSubmissions;

	auto lock = std::lock_guard(swapchain.dev->mutex);
	++swapchain.presentCounter;
	keepAliveFrameSubmissions = std::move(swapchain.frameSubmissions.back());

	for(auto i = swapchain.frameSubmissions.size(); i >= 2u; --i) {
		swapchain.frameSubmissions[i - 1] = std::move(swapchain.frameSubmissions[i - 2]);
	}

	swapchain.frameSubmissions[0] = std::move(swapchain.nextFrameSubmissions);
	swapchain.frameSubmissions[0].presentID = swapchain.presentCounter;
	swapchain.frameSubmissions[0].submissionEnd = swapchain.dev->submissionCounter;

	swapchain.nextFrameSubmissions = {};
	swapchain.nextFrameSubmissions.submissionStart = swapchain.dev->submissionCounter + 1;

	// timing
	auto now = Swapchain::Clock::now();
	if(swapchain.lastPresent) {
		if(swapchain.frameTimings.size() == swapchain.maxFrameTimings) {
			swapchain.frameTimings.erase(swapchain.frameTimings.begin());
		}

		auto timing = now - *swapchain.lastPresent;
		swapchain.frameTimings.push_back(timing);
	}
	swapchain.lastPresent = now;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(
		VkQueue                                     queue,
		const VkPresentInfoKHR*                     pPresentInfo) {
	ZoneScoped;

	auto& qd = getData<Queue>(queue);
	auto& dev = *qd.dev;

	// patch and submit semaphores
	// TODO: not sure submitting these on the queue and then presenting
	// is the same, probably not. Need to rework gui to not do present
	// itself and do batch forward in the end instead.
	/*
	ThreadMemScope tms;
	auto sems = tms.alloc<VkSemaphore>(pPresentInfo->waitSemaphoreCount);
	auto topOfPipes = tms.alloc<VkPipelineStageFlags>(pPresentInfo->waitSemaphoreCount);
	for(auto i = 0u; i < pPresentInfo->waitSemaphoreCount; ++i) {
		auto& semaphore = get(dev, pPresentInfo->pWaitSemaphores[i]);

		sems[i] = semaphore.handle;
		topOfPipes[i] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

		dlg_assert_or(!semaphore.signals.empty(), continue);
		dlg_assert(!semaphore.signals.back()->counterpart);

		if(semaphore.signals.back() == &SyncOp::swapchainAcquireDummy) {
			// acquire operation, no need to store it further
			semaphore.signals.pop_back();
		} else {
			semaphore.signals.back()->counterpart = &SyncOp::queuePresentDummy;
		}
	}

	if(!sems.empty()) {
		VkSubmitInfo si {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.pWaitSemaphores = sems.data();
		si.waitSemaphoreCount = sems.size();
		si.pWaitDstStageMask = topOfPipes.data();

		std::lock_guard lock(dev.queueMutex);
		ZoneScopedN("submit");
		dev.dispatch.QueueSubmit(queue, 1u, &si, VK_NULL_HANDLE);
	}
	*/

	// would need to figure out sync in this case
	dlg_assertm(pPresentInfo->swapchainCount == 1u,
		"Multipresent not supported atm");

	// dispatch presents, separately
	auto combinedResult = VK_SUCCESS;
	for(auto i = 0u; i < pPresentInfo->swapchainCount; ++i) {
		auto& swapchain = dev.swapchains.get(pPresentInfo->pSwapchains[i]);
		VkResult res;

		dlg_check({
			auto it = static_cast<const VkBaseInStructure*>(pPresentInfo->pNext);
			while (it) {
				dlg_warn("unhandled queue present pNext {}", it->sType);
				it = it->pNext;
			}
		});

		bool visible {};
		if(swapchain.overlay && swapchain.overlay->platform) {
			auto state = swapchain.overlay->platform->update(*swapchain.overlay->gui);
			visible = (state != Platform::State::hidden);

			std::lock_guard lock(dev.mutex);
			swapchain.overlay->gui->visible(visible);
		} else if(swapchain.overlay) {
			std::lock_guard lock(dev.mutex);
			visible = swapchain.overlay->gui->visible();
		}

		// update tracked frameSubmissions and timings before drawing
		// the potential overlay is important so that the new state
		// can already be displayed.
		swapchainPresent(swapchain);

		if(swapchain.overlay && visible) {
			span<const VkSemaphore> waitSems;
			if(i == 0u) {
				waitSems = {
					pPresentInfo->pWaitSemaphores,
					pPresentInfo->waitSemaphoreCount
				};
			} else {
				dlg_warn("Having to present without wait semaphores");
			}

			res = swapchain.overlay->drawPresent(qd, waitSems,
				pPresentInfo->pImageIndices[i]);
		} else {
			VkPresentInfoKHR pi {};
			pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			pi.pImageIndices = &pPresentInfo->pImageIndices[i];
			pi.pSwapchains = &pPresentInfo->pSwapchains[i];
			pi.swapchainCount = 1u;
			if(i == 0u) {
				pi.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
				pi.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
			} else if(pPresentInfo->waitSemaphoreCount) {
				dlg_warn("Having to present without wait semaphores");
			}
			pi.pNext = pPresentInfo->pNext;

			std::lock_guard queueLock(qd.dev->queueMutex);
			ZoneScopedN("dispatch");
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

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(
		VkDevice                                    device,
		VkSwapchainKHR                              vkSwapchain,
		uint32_t*                                   pSwapchainImageCount,
		VkImage*                                    pSwapchainImages) {
	auto& swapchain = get(device, vkSwapchain);
	auto& dev = *swapchain.dev;

	// Doing this quick forward here is nice so that with VIL_WRAP_IMAGE=0,
	// we will just forward instead of executing all the custom logic below (
	// that might e.g. not handle all cases of future extensions)
	if(!HandleDesc<VkImage>::wrap) {
		return dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapchain.handle,
			pSwapchainImageCount, pSwapchainImages);
	}

	auto res = VK_SUCCESS;
	if(pSwapchainImages) {
		auto writeCount = std::min<u32>(*pSwapchainImageCount, swapchain.images.size());
		for(auto i = 0u; i < writeCount; ++i) {
			pSwapchainImages[i] = castDispatch<VkImage>(*swapchain.images[i]);
		}
		*pSwapchainImageCount = writeCount;
		if(writeCount < swapchain.images.size()) {
			res = VK_INCOMPLETE;
		}
	} else {
		*pSwapchainImageCount = swapchain.images.size();
	}

	return res;
}

FrameSubmission::FrameSubmission() = default;
FrameSubmission::~FrameSubmission() = default;

} // namespace vil
