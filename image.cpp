#include "image.hpp"
#include "data.hpp"

namespace fuen {

// Image
VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
		VkDevice                                   	device,
		const VkImageCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImage*                                    pImage) {
	auto& dev = getData<Device>(device);

	// TODO: check if sampling is supported for this image
	auto ici = *pCreateInfo;
	ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

	auto res = dev.dispatch.vkCreateImage(device, &ici, pAllocator, pImage);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& img = dev.images.add(*pImage);
	img.dev = &dev;
	img.handle = *pImage;
	img.ci = ici;
	img.pendingLayout = ici.initialLayout;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
		VkDevice                                    device,
		VkImage                                     image,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = *findData<Device>(device);
	auto img = dev.images.mustMove(image);

	// TODO: messy. Use a callback system?
	// Or just remember VkImage as selected one in those instances? But
	// that might create problems when image handle is reused
	{
		std::lock_guard guard(dev.mutex);
		for(auto& swapchain : dev.swapchains.map) {
			if(!swapchain.second->useOverlay) {
				continue;
			}

			auto& renderer = swapchain.second->overlay.renderer;
			if(renderer.selected.image == img.get()) {
				renderer.selected.image = nullptr;
				if(renderer.selected.view) {
					dev.dispatch.vkDestroyImageView(dev.dev, renderer.selected.view, nullptr);
					renderer.selected.view = {};
				}
			}
		}

		auto& renderer = dev.window.renderer;
		if(renderer.selected.image == img.get()) {
			renderer.selected.image = nullptr;
			if(renderer.selected.view) {
				dev.dispatch.vkDestroyImageView(dev.dev, renderer.selected.view, nullptr);
				renderer.selected.view = {};
			}
		}
	}

	dev.dispatch.vkDestroyImage(device, image, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(
		VkDevice                                    device,
		VkImage                                     image,
		VkDeviceMemory                              memory,
		VkDeviceSize                                memoryOffset) {
	auto& dev = getData<Device>(device);
	auto& img = dev.images.get(image);
	auto& mem = dev.deviceMemories.get(memory);

	dlg_assert(!img.memory);

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.vkGetImageMemoryRequirements(dev.dev, image, &memReqs);

	img.memory = &mem;
	img.allocationOffset = memoryOffset;
	img.allocationSize = memReqs.size;
	mem.allocations.insert({memoryOffset, memReqs.size, &img});

	return dev.dispatch.vkBindImageMemory(device, image, memory, memoryOffset);
}

// ImageView
VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
		VkDevice                                    device,
		const VkImageViewCreateInfo*                pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImageView*                                pView) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateImageView(device, pCreateInfo, pAllocator, pView);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& view = dev.imageViews.add(*pView);
	view.view = *pView;
	view.img = dev.images.find(pCreateInfo->image);
	view.dev = &dev;
	view.ci = *pCreateInfo;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
		VkDevice                                    device,
		VkImageView                                 imageView,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = *findData<Device>(device);
	dev.imageViews.mustErase(imageView);
	dev.dispatch.vkDestroyImageView(device, imageView, pAllocator);
}


} // namespace fuen
