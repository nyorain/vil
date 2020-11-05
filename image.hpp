#pragma once

#include "common.hpp"

namespace fuen {

struct Image {
	Device* dev;
	VkImage image;
	std::string name;
	VkImageCreateInfo ci;

	// when they belong to a swapchain
	Swapchain* swapchain {};
};

struct ImageView {
	Device* dev;
	Image* img;
	VkImageView view;
	std::string name;
	VkImageViewCreateInfo ci;
};


VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
	VkDevice                                   	device,
	const VkImageCreateInfo*                    pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkImage*                                    pImage);

VKAPI_ATTR void VKAPI_CALL DestroyImage(
	VkDevice                                    device,
	VkImage                                     image,
	const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
    VkDevice                                    device,
    const VkImageViewCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkImageView*                                pView);

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
    VkDevice                                    device,
    VkImageView                                 imageView,
    const VkAllocationCallbacks*                pAllocator);

} // namespace fuen
