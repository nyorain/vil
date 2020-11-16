#include "image.hpp"
#include "data.hpp"
#include "gui.hpp"

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
	img.memoryResourceType = MemoryResource::Type::image;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
		VkDevice                                    device,
		VkImage                                     image,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = *findData<Device>(device);

	{
		auto img = dev.images.mustMove(image);

		// Unset selection if needed
		std::lock_guard lock(dev.mutex);
		auto unsetter = [&](Renderer& renderer) {
			if(renderer.selected.image.handle == img->handle) {
				if(renderer.selected.image.view) {
					dev.dispatch.vkDestroyImageView(device, renderer.selected.image.view, nullptr);
				}

				renderer.selected.image = {};
			}
		};
		forEachRenderer(dev, unsetter);

		// important that the destructor is run while mutex is locked,
		// see ~DeviceHandle
		img.reset();
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
	dev.dispatch.vkGetImageMemoryRequirements(device, image, &memReqs);

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
	view.handle = *pView;
	view.img = dev.images.find(pCreateInfo->image);
	view.dev = &dev;
	view.ci = *pCreateInfo;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
		VkDevice                                    device,
		VkImageView                                 imageView,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.imageViews.mustErase(imageView);
	dev.dispatch.vkDestroyImageView(device, imageView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(
		VkDevice                                    device,
		const VkSamplerCreateInfo*                  pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSampler*                                  pSampler) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& view = dev.samplers.add(*pSampler);
	view.dev = &dev;
	view.handle = *pSampler;
	view.ci = *pCreateInfo;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySampler(
		VkDevice                                    device,
		VkSampler                                   sampler,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.samplers.mustErase(sampler);
	dev.dispatch.vkDestroySampler(device, sampler, pAllocator);
}

} // namespace fuen
