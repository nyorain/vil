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
	img.image = *pImage;
	img.ci = ici;
	img.pendingLayout = ici.initialLayout;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
		VkDevice                                    device,
		VkImage                                     image,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = *findData<Device>(device);
	dev.images.mustErase(image);
	dev.dispatch.vkDestroyImage(device, image, pAllocator);
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
