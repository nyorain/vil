#pragma once

#include <fwd.hpp>
#include <memory.hpp>
#include <imageLayout.hpp>

namespace vil {

struct Image : MemoryResource {
	static constexpr auto objectType = VK_OBJECT_TYPE_IMAGE;

	VkImage handle {};
	VkImageCreateInfo ci;

	// resource references
	std::vector<ImageView*> views; // TODO: unordered set?

	// when images belongs to a swapchain
	Swapchain* swapchain {};
	u32 swapchainImageID {};

	bool externalMemory {}; // whether it supports importing/exporting

	// Properties related to our changes
	bool allowsNearestSampling {};
	bool allowsLinearSampling {};
	bool concurrentHooked {}; // whether we moved it into concurrent sharing mode
	bool hasTransferSrc {}; // whether we were able to set transferSrc usage

	// Device mutex must be locked and returned span only accessed
	// while it's locked.
	span<const ImageSubresourceLayout> pendingLayoutLocked() const;
	void initLayout();
	void applyLocked(span<const ImageSubresourceLayout>);
	void onApiDestroy();

	~Image();

private:
	// The image layout this image will have when *all* activated and pending
	// submissions are completed. When there are no pending submissions using
	// this image, it's the current layout.
	// Synced using device mutex.
	std::vector<ImageSubresourceLayout> pendingLayout_;
};

struct ImageView : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_IMAGE_VIEW;

	Image* img {}; // TODO: IntrusivePtr?
	VkImageView handle {};
	VkImageViewCreateInfo ci;

	std::vector<Framebuffer*> fbs; // TODO: unordered set?

	~ImageView();
};

struct Sampler : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_SAMPLER;

	VkSampler handle {};
	VkSamplerCreateInfo ci;
};

std::string defaultName(const Image&);
std::string defaultName(const ImageView&);

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

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(
    VkDevice                                    device,
    VkImage                                     image,
    VkMemoryRequirements*                       pMemoryRequirements);

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements(
    VkDevice                                    device,
    VkImage                                     image,
    uint32_t*                                   pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements*            pSparseMemoryRequirements);

VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(
    VkDevice                                    device,
    VkImage                                     image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceLayout*                        pLayout);

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(
    VkDevice                                    device,
    const VkImageMemoryRequirementsInfo2*       pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements);

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements2(
    VkDevice                                    device,
    const VkImageSparseMemoryRequirementsInfo2* pInfo,
    uint32_t*                                   pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2*           pSparseMemoryRequirements);

VKAPI_ATTR VkResult VKAPI_CALL GetImageDrmFormatModifierPropertiesEXT(
    VkDevice                                    device,
    VkImage                                     image,
    VkImageDrmFormatModifierPropertiesEXT*      pProperties);

// imageView
VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
    VkDevice                                    device,
    const VkImageViewCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkImageView*                                pView);

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
    VkDevice                                    device,
    VkImageView                                 imageView,
    const VkAllocationCallbacks*                pAllocator);

// sampler
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
