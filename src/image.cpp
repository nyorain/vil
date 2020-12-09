#include <image.hpp>
#include <layer.hpp>
#include <data.hpp>
#include <ds.hpp>
#include <rp.hpp>

namespace fuen {

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

	// Don't use for-loop as the descriptors unregister themselves
	while(!this->descriptors.empty()) {
		auto ref = this->descriptors[0];
		dlg_assert(ref.ds->getImageView(ref.binding, ref.elem) == this);
		ref.ds->invalidateLocked(ref.binding, ref.elem);
	}
}

Sampler::~Sampler() {
	if(!dev) {
		return;
	}

	// Don't use for-loop as the descriptors unregister themselves
	while(!this->descriptors.empty()) {
		auto ref = this->descriptors[0];
		dlg_assert(ref.ds->getSampler(ref.binding, ref.elem) == this);
		ref.ds->invalidateLocked(ref.binding, ref.elem);
	}
}

// Image
VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
		VkDevice                                   	device,
		const VkImageCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImage*                                    pImage) {
	auto& dev = getData<Device>(device);

	auto nci = *pCreateInfo;

	// If supported, we add the sampled flags to usage so we can
	// display it directly in our gui.
	VkFormatProperties props {};
	dev.ini->dispatch.GetPhysicalDeviceFormatProperties(dev.phdev,
		nci.format, &props);
	auto features = nci.tiling == VK_IMAGE_TILING_OPTIMAL ? props.optimalTilingFeatures : props.linearTilingFeatures;
	auto samplerType = Image::SamplerType::none;
	if(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
		nci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		samplerType = Image::SamplerType::nearest;

		if(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) {
			nci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			samplerType = Image::SamplerType::linear;
		}
	}

	// TODO: needed for our own operations on the buffer. We should
	// properly acquire/release it instead though, this might have
	// a performance impact.
	if(dev.usedQueueFamilyIndices.size() > 1) {
		nci.sharingMode = VK_SHARING_MODE_CONCURRENT;
		nci.queueFamilyIndexCount = dev.usedQueueFamilyIndices.size();
		nci.pQueueFamilyIndices = dev.usedQueueFamilyIndices.data();
	}

	auto res = dev.dispatch.CreateImage(device, &nci, pAllocator, pImage);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& img = dev.images.add(*pImage);
	img.objectType = VK_OBJECT_TYPE_IMAGE;
	img.dev = &dev;
	img.handle = *pImage;
	img.ci = *pCreateInfo;
	img.pendingLayout = pCreateInfo->initialLayout;
	img.samplerType = samplerType;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(
		VkDevice                                    device,
		VkImage                                     image,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = *findData<Device>(device);
	dev.images.mustErase(image);
	dev.dispatch.DestroyImage(device, image, pAllocator);
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
	dev.dispatch.GetImageMemoryRequirements(device, image, &memReqs);

	img.memory = &mem;
	img.allocationOffset = memoryOffset;
	img.allocationSize = memReqs.size;

	{
		// access to the given memory must be internally synced
		std::lock_guard lock(dev.mutex);
		mem.allocations.insert(&img);
	}

	return dev.dispatch.BindImageMemory(device, image, memory, memoryOffset);
}

// ImageView
VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(
		VkDevice                                    device,
		const VkImageViewCreateInfo*                pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImageView*                                pView) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateImageView(device, pCreateInfo, pAllocator, pView);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& view = dev.imageViews.add(*pView);
	view.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
	view.handle = *pView;
	view.img = dev.images.find(pCreateInfo->image);
	view.dev = &dev;
	view.ci = *pCreateInfo;

	{
		std::lock_guard lock(dev.mutex);
		view.img->views.push_back(&view);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(
		VkDevice                                    device,
		VkImageView                                 imageView,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.imageViews.mustErase(imageView);
	dev.dispatch.DestroyImageView(device, imageView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(
		VkDevice                                    device,
		const VkSamplerCreateInfo*                  pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkSampler*                                  pSampler) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateSampler(device, pCreateInfo, pAllocator, pSampler);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& view = dev.samplers.add(*pSampler);
	view.dev = &dev;
	view.handle = *pSampler;
	view.ci = *pCreateInfo;
	view.objectType = VK_OBJECT_TYPE_SAMPLER;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySampler(
		VkDevice                                    device,
		VkSampler                                   sampler,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.samplers.mustErase(sampler);
	dev.dispatch.DestroySampler(device, sampler, pAllocator);
}

} // namespace fuen
