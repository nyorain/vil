#pragma once

#include <device.hpp>
#include <memory.hpp>
#include <unordered_set>

namespace fuen {

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

	// Which samples should be used to display it.
	enum class SamplerType {
		none, // when an image can't be sampled
		linear,
		nearest,
	};

	SamplerType samplerType {SamplerType::none};

	~Image();
};

struct ImageView : DeviceHandle {
	Image* img {};
	VkImageView handle {};
	VkImageViewCreateInfo ci;

	std::vector<Framebuffer*> fbs; // TODO: unordered set?
	std::unordered_set<DescriptorSetRef, DescriptorSetRef::Hash> descriptors;

	~ImageView();
};

struct Sampler : DeviceHandle {
	VkSampler handle {};
	VkSamplerCreateInfo ci;

	std::unordered_set<DescriptorSetRef, DescriptorSetRef::Hash> descriptors;

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

} // namespace fuen
