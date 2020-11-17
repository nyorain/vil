#pragma once

#include "device.hpp"
#include "memory.hpp"

namespace fuen {

struct Image : MemoryResource {
	VkImage handle;
	VkImageCreateInfo ci;

	// The image layout this image will have when *all* pending submissions
	// are completed. When there are no pending submissions using this
	// image, it's the current layout.
	VkImageLayout pendingLayout {VK_IMAGE_LAYOUT_UNDEFINED};

	// resource references
	std::vector<ImageView*> views;
	std::vector<Framebuffer*> fbs;
	// std::vector<DescriptorSet*> descriptors;
	// std::vector<CommandBuffer*> commandBuffers;

	// when images belongs to a swapchain
	Swapchain* swapchain {};
	u32 swapchainImageID {};
};

struct ImageView : DeviceHandle {
	Image* img {};
	VkImageView handle {};
	VkImageViewCreateInfo ci;
};

struct Sampler : DeviceHandle {
	VkSampler handle {};
	VkSamplerCreateInfo ci;
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

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(
    VkDevice                                    device,
    VkImage                                     image,
    VkDeviceMemory                              memory,
    VkDeviceSize                                memoryOffset);

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
    VkDevice                                    device,
    const VkImageViewCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkImageView*                                pView);

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
    VkDevice                                    device,
    VkImageView                                 imageView,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(
    VkDevice                                    device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSampler*                                  pSampler);

VKAPI_ATTR void VKAPI_CALL DestroySampler(
    VkDevice                                    device,
    VkSampler                                   sampler,
    const VkAllocationCallbacks*                pAllocator);

} // namespace fuen
