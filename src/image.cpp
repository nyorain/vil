#include <image.hpp>
#include <wrap.hpp>
#include <layer.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <data.hpp>
#include <threadContext.hpp>
#include <ds.hpp>
#include <rp.hpp>
#include <util/util.hpp>
#include <vkutil/enumString.hpp>

namespace vil {

static_assert(validExpression<HasOnApiDestroy, Image>);
static_assert(validExpression<HasOnApiDestroy, DeviceMemory>);

// Classes
Image::~Image() = default;

void Image::onApiDestroy() {
	MemoryResource::onApiDestroy();

	std::lock_guard lock(dev->mutex);
	for(auto* view : this->views) {
		view->img = nullptr;
	}
}

span<const ImageSubresourceLayout> Image::pendingLayoutLocked() const {
	assertOwned(dev->mutex);
	return pendingLayout_;
}

void Image::applyLocked(span<const ImageSubresourceLayout> changes) {
	apply(pendingLayout_, changes);

	// TODO: always do this here?
	// simplify(pendingLayout_);

	dlg_check(checkForErrors(pendingLayout_, ci););
}

void Image::initLayout() {
	auto& fullSubres = pendingLayout_.emplace_back();
	fullSubres.range.aspectMask = aspects(ci.format);
	fullSubres.range.baseArrayLayer = 0u;
	fullSubres.range.baseMipLevel = 0u;
	fullSubres.range.layerCount = ci.arrayLayers;
	fullSubres.range.levelCount = ci.mipLevels;
	fullSubres.layout = ci.initialLayout;
}

ImageView::~ImageView() {
	if(!dev) { // can happen for serialized handles
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

std::string defaultName(const Image& img) {
	// format
	auto formatStr = vk::name(img.ci.format);

	// extent
	std::string dimStr;
	if(img.ci.imageType == VK_IMAGE_TYPE_1D) {
		// We will print just a number e.g. 1024 in this case.
		// Might be confusing but it's consistent with the rest,
		// the user will figure it out.
		dimStr = dlg::format("{}", img.ci.extent.width);
	} else if(img.ci.imageType == VK_IMAGE_TYPE_2D) {
		dimStr = dlg::format("{}x{}", img.ci.extent.width, img.ci.extent.height);
	} else if(img.ci.imageType == VK_IMAGE_TYPE_3D) {
		dimStr = dlg::format("{}x{}x{}", img.ci.extent.width,
			img.ci.extent.height, img.ci.extent.depth);
	} else {
		dimStr = "<unsupported type>";
	}

	if(img.ci.arrayLayers > 1) {
		dimStr += dlg::format("[{}]", img.ci.arrayLayers);
	}

	if(img.ci.samples > 1) {
		dimStr += dlg::format("<{}>", img.ci.samples);
	}

	auto ret = dlg::format("{} {}", formatStr, dimStr);

	// usage
	auto attachmentUsage =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if(img.ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) {
		ret += " Storage";
	} else if(img.ci.usage & attachmentUsage) {
		ret += " Attachment";
	} else if(img.ci.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
		ret += " Sampled";
	}

	return ret;
}

std::string defaultName(const ImageView& iv) {
	if(!iv.img) {
		return "View to Destroy Image";
	}

	// format
	auto formatStr = vk::name(iv.ci.format);

	// extent
	std::string dimStr;
	if(iv.ci.viewType == VK_IMAGE_VIEW_TYPE_1D || iv.ci.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY) {
		// We will print just a number e.g. 1024 in this case.
		// Might be confusing but it's consistent with the rest,
		// the user will figure it out.
		dimStr = dlg::format("{}", iv.img->ci.extent.width);
	} else if(iv.ci.viewType == VK_IMAGE_VIEW_TYPE_2D || iv.ci.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
		dimStr = dlg::format("{}x{}", iv.img->ci.extent.width,
			iv.img->ci.extent.height);
	} else if(iv.ci.viewType == VK_IMAGE_VIEW_TYPE_3D) {
		dimStr = dlg::format("{}x{}x{}", iv.img->ci.extent.width,
			iv.img->ci.extent.height, iv.img->ci.extent.depth);
	} else if(iv.ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE || iv.ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
		dimStr = dlg::format("Cube {}x{}", iv.img->ci.extent.width,
			iv.img->ci.extent.height);
	} else {
		dimStr = "<unsupported type>";
	}

	// layers
	const auto layered =
		iv.ci.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
		iv.ci.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
		iv.ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
	if(layered) {
		dimStr += dlg::format("[{}:{}]",
			iv.ci.subresourceRange.baseArrayLayer,
			iv.ci.subresourceRange.baseArrayLayer + iv.ci.subresourceRange.layerCount);
	} else if(iv.ci.subresourceRange.baseArrayLayer != 0u) {
		dlg_assert(iv.ci.subresourceRange.layerCount == 1u);
		dimStr += dlg::format("[{}]", iv.ci.subresourceRange.baseArrayLayer);
	}

	// levels
	// XXX: always show it? or just hide it when it's only the only base level?
	if(iv.ci.subresourceRange.baseMipLevel != 0u ||
			iv.ci.subresourceRange.levelCount != iv.img->ci.mipLevels) {
		dimStr += dlg::format("[mip {}:{}]",
			iv.ci.subresourceRange.baseMipLevel,
			iv.ci.subresourceRange.baseMipLevel + iv.ci.subresourceRange.levelCount);
	}

	// TODO: show aspect?

	// XXX: Really output samples here? already so much clutter
	if(iv.img->ci.samples > 1) {
		dimStr += dlg::format("<{}>", iv.img->ci.samples);
	}

	auto ret = dlg::format("{} {}", formatStr, dimStr);

	// XXX: don't output usage here? already so much clutter.
	// And might be confusing, storage image view might be used as sampled
	// image.
	// auto attachmentUsage =
	// 	VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	// 	VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	// if(img.ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) {
	// 	ret += " Storage";
	// } else if(img.ci.usage & attachmentUsage) {
	// 	ret += " Attachment";
	// } else if(img.ci.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
	// 	ret += " Sampled";
	// }

	return ret;
}

// Image
VKAPI_ATTR VkResult VKAPI_CALL CreateImage(
		VkDevice                                   	device,
		const VkImageCreateInfo*                    pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkImage*                                    pImage) {
	auto& dev = getDevice(device);

	auto nci = *pCreateInfo;

	bool linearSampling {};
	bool nearestSampling {};
	bool concurrent {};

#ifndef VIL_NO_IMAGE_MOD
	const auto isTransient = !!(nci.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);

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
#endif // VIL_NO_SWAPCHAIN_MOD

	auto res = dev.dispatch.CreateImage(dev.handle, &nci, pAllocator, pImage);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto imgPtr = IntrusivePtr<Image>(new Image());
	auto& img = *imgPtr;
	img.memObjectType = VK_OBJECT_TYPE_IMAGE;
	img.dev = &dev;
	img.handle = *pImage;
	img.ci = *pCreateInfo;
	img.allowsNearestSampling = nearestSampling;
	img.allowsLinearSampling = linearSampling;
	img.concurrentHooked = concurrent;
	img.hasTransferSrc = nci.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	img.initLayout();

	if(img.ci.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
		img.memory = SparseMemoryState{};
	}

	constexpr auto sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	auto* externalMem = findChainInfo<VkExternalMemoryImageCreateInfo, sType>(nci);
	if(externalMem && externalMem->handleTypes) {
		img.externalMemory = true;
	}

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

	auto& dev = *mustMoveUnset(device, image)->dev;
	dev.dispatch.DestroyImage(dev.handle, image, pAllocator);
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
	dlg_assert(img.memory.index() == 0u);
	auto& memBind = std::get<0>(img.memory);

	dlg_assert(!memBind.memory);
	dlg_assert(memBind.memState == FullMemoryBind::State::unbound);

	memBind.memory = &mem;
	memBind.memOffset = offset;
	memBind.memSize = memReqs.size;
	memBind.memState = FullMemoryBind::State::bound;
	memBind.resource = &img;

	mem.allocations.insert(&memBind);
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
		dlg_assert(bind.image);
		auto& img = get(dev, bind.image);

		fwd[i] = bind;
		fwd[i].image = img.handle;

		// can be VK_NULL_HANDLE, e.g. for VkBindImageMemorySwapchainInfoKHR
		if(bind.memory) {
			auto& mem = get(dev, bind.memory);
			bindImageMemory(img, mem, bind.memoryOffset);
			fwd[i].memory = mem.handle;
		}
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
	view.handle = *pView;
	view.img = &img;
	view.dev = &dev;
	view.ci = *pCreateInfo;

	// TODO take special rules for 3D image as 2d array view and such
	// into account, see docs on vkCreateImageView
	if(view.ci.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS) {
		view.ci.subresourceRange.layerCount =
			img.ci.arrayLayers - view.ci.subresourceRange.baseArrayLayer;
	}

	if(view.ci.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS) {
		view.ci.subresourceRange.levelCount =
			img.ci.mipLevels - view.ci.subresourceRange.baseMipLevel;
	}

	{
		std::lock_guard lock(dev.mutex);
		view.img->views.push_back(&view);
	}

	*pView = castDispatch<VkImageView>(view);
	dev.imageViews.mustEmplace(std::move(viewPtr));

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

	auto& dev = mustMoveUnsetKeepAlive<&Device::keepAliveImageViews>(device, imageView);
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

	*pSampler = castDispatch<VkSampler>(sampler);
	dev.samplers.mustEmplace(std::move(samplerPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroySampler(
		VkDevice                                    device,
		VkSampler                                   handle,
		const VkAllocationCallbacks*                pAllocator) {
	if(!handle) {
		return;
	}

	auto& dev = mustMoveUnsetKeepAlive<&Device::keepAliveSamplers>(device, handle);
	dev.dispatch.DestroySampler(dev.handle, handle, pAllocator);
}

} // namespace vil
