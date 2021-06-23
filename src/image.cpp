#include <image.hpp>
#include <layer.hpp>
#include <device.hpp>
#include <data.hpp>
#include <threadContext.hpp>
#include <ds.hpp>
#include <rp.hpp>

namespace vil {

// Classes
Image::~Image() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);
	for(auto* view : this->views) {
		view->img = nullptr;
	}
}

ImageView::~ImageView() {
	if(!dev) {
		return;
	}

	dlg_assert(DebugStats::get().aliveImagesViews > 0);
	--DebugStats::get().aliveImagesViews;

	std::lock_guard lock(dev->mutex);
	if(this->img) {
		auto it = std::find(this->img->views.begin(), this->img->views.end(), this);
		dlg_assert(it != this->img->views.end());
		this->img->views.erase(it);
	}

	for(auto* fb : this->fbs) {
		auto it = std::find(fb->attachments.begin(), fb->attachments.end(), this);
		dlg_assert(it != fb->attachments.end());
		fb->attachments.erase(it);
	}
}

Sampler::~Sampler() {
	if(!dev) {
		return;
	}
}

// Image
VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
		VkDevice                                   	device,
		const VkImageCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImage*                                    pImage) {
	auto& dev = getDevice(device);

	auto nci = *pCreateInfo;
	auto isTransient = !!(nci.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);

	bool linearSampling {};
	bool nearestSampling {};

	// If supported, we add the sampled flags to usage so we can
	// display it directly in our gui.
	VkFormatProperties props {};
	dev.ini->dispatch.GetPhysicalDeviceFormatProperties(dev.phdev, nci.format, &props);
	auto features = nci.tiling == VK_IMAGE_TILING_OPTIMAL ?
		props.optimalTilingFeatures :
		props.linearTilingFeatures;
	if((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !isTransient) {
		nci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		nearestSampling = true;

		if(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) {
			linearSampling = true;
		}
	}

	// NOTE: needed for our own operations on the buffer. Might be better to
	// properly acquire/release it instead though, this might have
	// a performance impact.
	auto concurrent = false;
	if(dev.usedQueueFamilyIndices.size() > 1 &&
			nci.sharingMode != VK_SHARING_MODE_CONCURRENT &&
			nearestSampling) {
		nci.sharingMode = VK_SHARING_MODE_CONCURRENT;
		nci.queueFamilyIndexCount = u32(dev.usedQueueFamilyIndices.size());
		nci.pQueueFamilyIndices = dev.usedQueueFamilyIndices.data();
		concurrent = true;
	}

	// The transferSrc format feature was only added in vulkan 1.1.
	// For vulkan 1.0 we can just assume it I guess.
	// We don't simply unset the transient attachment bit (it would make
	// sense as the memory needs to be non-transient anyways when we hook
	// the renderpass) since that makes things complicated: the memory
	// type changes, GetMemoryCommitment cannot be called and so on.
	// Would have to do a lot of work to fix the aftermath of that change.
	if((!dev.ini->vulkan11 || features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && !isTransient) {
		nci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	auto res = dev.dispatch.CreateImage(dev.handle, &nci, pAllocator, pImage);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto imgPtr = std::make_unique<Image>();
	auto& img = *imgPtr;
	img.objectType = VK_OBJECT_TYPE_IMAGE;
	img.dev = &dev;
	img.handle = *pImage;
	img.ci = *pCreateInfo;
	img.pendingLayout = pCreateInfo->initialLayout;
	img.allowsNearestSampling = nearestSampling;
	img.allowsLinearSampling = linearSampling;
	img.concurrentHooked = concurrent;
	img.hasTransferSrc = nci.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	*pImage = castDispatch<VkImage>(img);
	dev.images.mustEmplace(*pImage, std::move(imgPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
		VkDevice                                    device,
		VkImage                                     image,
		const VkAllocationCallbacks*                pAllocator) {
	if(!image) {
		return;
	}

	auto& dev = getDevice(device);
	auto handle = dev.images.mustMove(image)->handle;
	dev.dispatch.DestroyImage(dev.handle, handle, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(
		VkDevice                                    device,
		VkImage                                     image,
		VkMemoryRequirements*                       pMemoryRequirements) {
	auto& img = get(device, image);
	img.dev->dispatch.GetImageMemoryRequirements(img.dev->handle,
		img.handle, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements(
		VkDevice                                    device,
		VkImage                                     image,
		uint32_t*                                   pSparseMemoryRequirementCount,
		VkSparseImageMemoryRequirements*            pSparseMemoryRequirements) {
	auto& img = get(device, image);
	img.dev->dispatch.GetImageSparseMemoryRequirements(img.dev->handle,
		img.handle, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(
		VkDevice                                    device,
		VkImage                                     image,
		const VkImageSubresource*                   pSubresource,
		VkSubresourceLayout*                        pLayout) {
	auto& img = get(device, image);
	img.dev->dispatch.GetImageSubresourceLayout(img.dev->handle,
		img.handle, pSubresource, pLayout);
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(
		VkDevice                                    device,
		const VkImageMemoryRequirementsInfo2*       pInfo,
		VkMemoryRequirements2*                      pMemoryRequirements) {
	auto& img = get(device, pInfo->image);
	auto fwd = *pInfo;
	fwd.image = img.handle;
	img.dev->dispatch.GetImageMemoryRequirements2(img.dev->handle,
		&fwd, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements2(
		VkDevice                                    device,
		const VkImageSparseMemoryRequirementsInfo2* pInfo,
		uint32_t*                                   pSparseMemoryRequirementCount,
		VkSparseImageMemoryRequirements2*           pSparseMemoryRequirements) {
	auto& img = get(device, pInfo->image);
	auto fwd = *pInfo;
	fwd.image = img.handle;
	img.dev->dispatch.GetImageSparseMemoryRequirements2(img.dev->handle,
		&fwd, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL GetImageDrmFormatModifierPropertiesEXT(
		VkDevice                                    device,
		VkImage                                     image,
		VkImageDrmFormatModifierPropertiesEXT*      pProperties) {
	auto& img = get(device, image);
	return img.dev->dispatch.GetImageDrmFormatModifierPropertiesEXT(img.dev->handle,
		img.handle, pProperties);
}

void bindImageMemory(Image& img, DeviceMemory& mem, u64 offset) {
	auto& dev = *img.dev;

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetImageMemoryRequirements(dev.handle, img.handle, &memReqs);

	// access to the given memory and image must be internally synced
	std::lock_guard lock(dev.mutex);
	dlg_assert(!img.memory);
	dlg_assert(!img.memoryDestroyed);
	mem.allocations.push_back(&img);

	img.memory = &mem;
	img.allocationOffset = offset;
	img.allocationSize = memReqs.size;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(
		VkDevice                                    device,
		uint32_t                                    bindInfoCount,
		const VkBindImageMemoryInfo*                pBindInfos) {
	auto& dev = getDevice(device);

	ThreadMemScope memScope;
	auto fwd = memScope.alloc<VkBindImageMemoryInfo>(bindInfoCount);
	for(auto i = 0u; i < bindInfoCount; ++i) {
		auto& bind = pBindInfos[i];
		auto& img = get(dev, bind.image);
		auto& mem = get(dev, bind.memory);

		bindImageMemory(img, mem, bind.memoryOffset);

		fwd[i] = bind;
		fwd[i].image = img.handle;
		fwd[i].memory = mem.handle;
	}

	return dev.dispatch.BindImageMemory2(dev.handle, u32(fwd.size()), fwd.data());
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(
		VkDevice                                    device,
		VkImage                                     image,
		VkDeviceMemory                              memory,
		VkDeviceSize                                memoryOffset) {
	auto& img = get(device, image);
	auto& mem = get(device, memory);
	bindImageMemory(img, mem, memoryOffset);
	return img.dev->dispatch.BindImageMemory(img.dev->handle, img.handle,
		mem.handle, memoryOffset);
}

// ImageView
VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
		VkDevice                                    device,
		const VkImageViewCreateInfo*                pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImageView*                                pView) {
	auto& img = get(device, pCreateInfo->image);
	auto& dev = *img.dev;

	auto fwd = *pCreateInfo;
	fwd.image = img.handle;

	auto res = dev.dispatch.CreateImageView(dev.handle, &fwd, pAllocator, pView);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto viewPtr = IntrusivePtr<ImageView>(new ImageView());
	auto& view = *viewPtr;
	view.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
	view.handle = *pView;
	view.img = &img;
	view.dev = &dev;
	view.ci = *pCreateInfo;

	{
		std::lock_guard lock(dev.mutex);
		view.img->views.push_back(&view);
	}

	*pView = castDispatch<VkImageView>(view);
	dev.imageViews.mustEmplace(*pView, std::move(viewPtr));

	++DebugStats::get().aliveImagesViews;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
		VkDevice                                    device,
		VkImageView                                 imageView,
		const VkAllocationCallbacks*                pAllocator) {
	if(!imageView) {
		return;
	}

	auto& dev = getDevice(device);
	mustMoveUnset(dev, imageView);
	dev.dispatch.DestroyImageView(dev.handle, imageView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(
		VkDevice                                    device,
		const VkSamplerCreateInfo*                  pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSampler*                                  pSampler) {
	auto& dev = getDevice(device);

	auto res = dev.dispatch.CreateSampler(dev.handle, pCreateInfo, pAllocator, pSampler);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto samplerPtr = IntrusivePtr<Sampler>(new Sampler());
	auto& sampler = *samplerPtr;
	sampler.dev = &dev;
	sampler.handle = *pSampler;
	sampler.ci = *pCreateInfo;
	sampler.objectType = VK_OBJECT_TYPE_SAMPLER;

	*pSampler = castDispatch<VkSampler>(sampler);
	dev.samplers.mustEmplace(*pSampler, std::move(samplerPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySampler(
		VkDevice                                    device,
		VkSampler                                   sampler,
		const VkAllocationCallbacks*                pAllocator) {
	if(!sampler) {
		return;
	}

	auto& dev = getDevice(device);
	mustMoveUnset(dev, sampler);
	dev.dispatch.DestroySampler(dev.handle, sampler, pAllocator);
}

} // namespace vil
