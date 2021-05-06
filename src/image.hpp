#pragma once

#include <fwd.hpp>
#include <memory.hpp>
#include <unordered_set>

namespace vil {

struct Image : MemoryResource {
	VkImage handle {};
	VkImageCreateInfo ci;

	// The image layout this image will have when *all* pending submissions
	// are completed. When there are no pending submissions using this
	// image, it's the current layout.
	VkImageLayout pendingLayout {VK_IMAGE_LAYOUT_UNDEFINED};

	// resource references
	std::vector<ImageView*> views; // TODO: unordered set?

	// when images belongs to a swapchain
	Swapchain* swapchain {};
	u32 swapchainImageID {};

	// Properties related to our changes
	bool allowsNearestSampling {};
	bool allowsLinearSampling {};
	bool concurrentHooked {}; // whether we moved it into concurrent sharing mode
	bool hasTransferSrc {}; // whether we were able to set transferSrc usage

	~Image();
};

struct ImageView : DeviceHandle {
	Image* img {};
	VkImageView handle {};
	VkImageViewCreateInfo ci;

	std::vector<Framebuffer*> fbs; // TODO: unordered set?

	~ImageView();
};

struct Sampler : DeviceHandle {
	VkSampler handle {};
	VkSamplerCreateInfo ci;

	~Sampler();
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

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(
    VkDevice                                    device,
    uint32_t                                    bindInfoCount,
    const VkBindImageMemoryInfo*                pBindInfos);

// no need to hook:
// - GetImageSubresourceLayout

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

} // namespace vil
