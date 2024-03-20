#include <commandHook/copy.hpp>
#include <commandHook/hook.hpp> // TODO: only for pipes
#include <commandHook/state.hpp>
#include <device.hpp>
#include <ds.hpp>
#include <image.hpp>
#include <layer.hpp>
#include <buffer.hpp>
#include <queue.hpp>
#include <threadContext.hpp>
#include <util/debugMutex.hpp>
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>

namespace vil {

VkFormat upgradeToSafeStorageTexelWithoutFormat(VkFormat src) {
	dlg_assert(!FormatIsCompressed(src));
	dlg_assert(!FormatIsMultiplane(src));
	dlg_assert(FormatIsColor(src));

	// TODO PERF: implement proper map
	// maybe just implement one central format map somewhere that
	// has everything we need? then we can rid of format utils as well.
	// Their map doesn't work for all our cases (e.g. format io) but
	// it's a good starting point I guess
	// auto numChannels = FormatComponentCount(src);
	// auto texelSize = FormatTexelSize(src);
	return VK_FORMAT_R32G32B32A32_SFLOAT;
}

VkFormat sampleFormat(VkFormat src, VkImageAspectFlagBits aspect) {
	// NOTE: see the vulkan table to formats that can be written
	// with the shaderStorageImageWriteWithoutFormat feature
	if(FormatIsCompressed(src) || FormatIsMultiplane(src)) {
		dlg_assert(aspect == VK_IMAGE_ASPECT_COLOR_BIT);

		switch(src) {
			case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
			case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
			case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
			case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
			case VK_FORMAT_BC2_SRGB_BLOCK:
			case VK_FORMAT_BC2_UNORM_BLOCK:
			case VK_FORMAT_BC3_SRGB_BLOCK:
			case VK_FORMAT_BC3_UNORM_BLOCK:
			case VK_FORMAT_BC4_UNORM_BLOCK:
			case VK_FORMAT_BC5_UNORM_BLOCK:
			case VK_FORMAT_BC7_SRGB_BLOCK:
			case VK_FORMAT_BC7_UNORM_BLOCK:
				return VK_FORMAT_R8G8B8A8_UNORM;
			case VK_FORMAT_BC4_SNORM_BLOCK:
			case VK_FORMAT_BC5_SNORM_BLOCK:
				return VK_FORMAT_R8G8B8A8_SNORM;
			case VK_FORMAT_BC6H_SFLOAT_BLOCK:
			case VK_FORMAT_BC6H_UFLOAT_BLOCK:
				return VK_FORMAT_R16G16B16A16_SFLOAT;
			default:
				break;
		}

		// TODO: overkill in many cases, figure out minimum format able
		// to hold the uncompressed data
		if(FormatIsSampledFloat(src)) {
			return VK_FORMAT_R32G32B32A32_SFLOAT;
		} else if(FormatIsSINT(src)) {
			return VK_FORMAT_R32G32B32A32_SINT;
		} else if(FormatIsUINT(src)) {
			return VK_FORMAT_R32G32B32A32_UINT;
		}
	}

	if(src == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ||
			src == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
		return VK_FORMAT_R16G16B16A16_SFLOAT;
	}

	switch(src) {
		case VK_FORMAT_D16_UNORM:
			return VK_FORMAT_R16_UNORM;
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT:
			return VK_FORMAT_R32_SFLOAT;
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			dlg_assert(aspect == VK_IMAGE_ASPECT_DEPTH_BIT ||
				aspect == VK_IMAGE_ASPECT_STENCIL_BIT);
			return aspect == VK_IMAGE_ASPECT_DEPTH_BIT ?
				VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8_UINT;
		case VK_FORMAT_D16_UNORM_S8_UINT:
			dlg_assert(aspect == VK_IMAGE_ASPECT_DEPTH_BIT ||
				aspect == VK_IMAGE_ASPECT_STENCIL_BIT);
			return aspect == VK_IMAGE_ASPECT_DEPTH_BIT ?
				VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UINT;
		case VK_FORMAT_S8_UINT:
			return VK_FORMAT_R8_UINT;
		default:
			break;
	}

	// TODO: a lot more cases to handle

	return src;
}

bool CopiedImage::init(Device& dev, VkFormat format, const VkExtent3D& extent,
		u32 layers, u32 levels, VkImageAspectFlags aspects, u32 srcQueueFam) {
	ZoneScoped;

	this->dev = &dev;
	this->extent = extent;
	this->levelCount = levels;
	this->layerCount = layers;
	this->aspectMask = aspects;
	this->format = format;

	// TODO: support multisampling?
	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.arrayLayers = layerCount;
	ici.extent = extent;
	ici.format = format;
	ici.imageType = minImageType(this->extent);
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // needed for texel reading later on

	std::array<u32, 2> qfams = {dev.gfxQueue->family, srcQueueFam};

	if(srcQueueFam == dev.gfxQueue->family) {
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	} else {
		// PERF: we could just perform an explicit transition in this case,
		//   it's really not hard here
		ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
		ici.pQueueFamilyIndices = qfams.data();
		ici.queueFamilyIndexCount = u32(qfams.size());
	}

	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.mipLevels = levelCount;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;

	// Before creating image, check if device supports everything we need.
	// We will not copy the image in that case but in almost all cases there
	// are workarounds (e.g. copy to other format, split size or array layers
	// or whatever) that we can implement when this really fails for someone
	// and some usecase we want to support. So we make sure we log every issue.
	VkImageFormatProperties fmtProps;
	auto res = dev.ini->dispatch.GetPhysicalDeviceImageFormatProperties(
		dev.phdev, ici.format, ici.imageType, ici.tiling,
		ici.usage, ici.flags, &fmtProps);
	if(res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		dlg_warn("CopiedImage: Unsupported format {} (imageType {}, "
			"tiling {}, usage {}, flags {}",
			vk::name(ici.format), vk::name(ici.imageType), vk::name(ici.tiling),
			vk::nameImageUsageFlags(ici.usage),
			vk::nameImageCreateFlags(ici.flags));
		return false;
	}

	if(ici.arrayLayers > fmtProps.maxArrayLayers) {
		dlg_warn("CopiedImage: only {} layers supported (needing {})",
			fmtProps.maxArrayLayers, ici.arrayLayers);
		return false;
	}

	if(ici.mipLevels > fmtProps.maxMipLevels) {
		dlg_warn("CopiedImage: only {} levels supported (needing {})",
			fmtProps.maxMipLevels, ici.mipLevels);
		return false;
	}

	if(ici.extent.width > fmtProps.maxExtent.width ||
			ici.extent.height > fmtProps.maxExtent.height ||
			ici.extent.depth > fmtProps.maxExtent.depth) {
		dlg_warn("CopiedImage: max supported size {}x{}x{} (needing {}x{}x{})",
			fmtProps.maxExtent.width, fmtProps.maxExtent.height, fmtProps.maxExtent.depth,
			ici.extent.width, ici.extent.height, ici.extent.depth);
		return false;
	}

	// Create image
	VK_CHECK_DEV(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &image), dev);
	nameHandle(dev, this->image, "CopiedImage:image");

	VkMemoryRequirements memReqs;
	dev.dispatch.GetImageMemoryRequirements(dev.handle, image, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	// NOTE: even though using host visible memory would make some operations
	//   eaiser (such as showing a specific texel value in gui), the guarantees
	//   vulkan gives for support of linear images are quite small.
	//   And on device with dgpu (which are probably our main target,
	//   at least what i'm interested in mainly), using host visible here
	//   means transfering all the data from gpu to cpu which would
	//   have a significant overhead.
	auto memBits = memReqs.memoryTypeBits & dev.deviceLocalMemTypeBits;
	allocInfo.memoryTypeIndex = findLSB(memBits);
	VK_CHECK_DEV(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &memory), dev);
	nameHandle(dev, this->memory, "CopiedImage:memory");

	VK_CHECK_DEV(dev.dispatch.BindImageMemory(dev.handle, image, memory, 0), dev);

	neededMemory = memReqs.size;
	DebugStats::get().copiedImageMem += memReqs.size;

	return true;
}

CopiedImage::~CopiedImage() {
	if(!dev) {
		return;
	}

	dev->dispatch.DestroyImage(dev->handle, image, nullptr);
	dev->dispatch.FreeMemory(dev->handle, memory, nullptr);

	DebugStats::get().copiedImageMem -= neededMemory;
}

// Initializes the given CopiedImage 'dst' and add commands to 'cb' (associated
// with queue family 'srcQueueFam') to copy the given 'srcSubres' from
// 'src' (which is in the given 'srcLayout') into 'dst'.
// After executing the recorded commands, 'dst' will contain srcSubres from src.
void initAndCopy(Device& dev, VkCommandBuffer cb, CopiedImage& dst, Image& src,
		VkImageLayout srcLayout, VkImageSubresourceRange srcSubres,
		u32 srcQueueFam) {
	(void) srcLayout;

	if(!src.hasTransferSrc) {
		// There are only very specific cases where this can happen,
		// we could work around some of them (e.g. transient
		// attachment images or swapchain images that don't
		// support transferSrc).
		dlg_warn("Can't display image copy; original can't be copied");
		return;
	}

	auto extent = src.ci.extent;
	for(auto i = 0u; i < srcSubres.baseMipLevel; ++i) {
		extent.width = std::max(extent.width >> 1, 1u);

		if(extent.height) {
			extent.height = std::max(extent.height >> 1, 1u);
		}

		if(extent.depth) {
			extent.depth = std::max(extent.depth >> 1, 1u);
		}
	}

	if(srcSubres.layerCount == VK_REMAINING_ARRAY_LAYERS) {
		srcSubres.layerCount = src.ci.arrayLayers - srcSubres.baseArrayLayer;
	}
	if(srcSubres.levelCount == VK_REMAINING_MIP_LEVELS) {
		srcSubres.levelCount = src.ci.mipLevels - srcSubres.baseMipLevel;
	}

	if(srcSubres.layerCount == 0u || srcSubres.levelCount == 0u ||
			extent.width == 0u || extent.height == 0u || extent.depth == 0u) {
		dlg_warn("Image copy would be empty");
		return;
	}

	auto success = dst.init(dev, src.ci.format, extent, srcSubres.layerCount,
		srcSubres.levelCount, srcSubres.aspectMask, srcQueueFam);
	if(!success) {
		dlg_warn("Initializing image copy failed");
		return;
	}

	// perform copy
	VkImageMemoryBarrier imgBarriers[2] {};

	auto& srcBarrier = imgBarriers[0];
	srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	srcBarrier.image = src.handle;
	srcBarrier.oldLayout = srcLayout;
	srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	srcBarrier.srcAccessMask =
		VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	srcBarrier.subresourceRange = srcSubres;
	srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	auto& dstBarrier = imgBarriers[1];
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.image = dst.image;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcAccessMask = 0u;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.subresourceRange.aspectMask = dst.aspectMask;
	dstBarrier.subresourceRange.layerCount = dst.layerCount;
	dstBarrier.subresourceRange.levelCount = dst.levelCount;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dunno, NOTE: probably could
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 2, imgBarriers);

	// for 1-sample images we can copy, otherwise we need resolve
	ThreadMemScope memScope;
	if(src.ci.samples == VK_SAMPLE_COUNT_1_BIT) {
		auto copies = memScope.alloc<VkImageCopy>(srcSubres.levelCount);
		for(auto m = 0u; m < srcSubres.levelCount; ++m) {
			auto& copy = copies[m];
			copy.dstOffset = {};
			copy.srcOffset = {};
			copy.extent = extent;
			copy.srcSubresource.aspectMask = srcSubres.aspectMask;
			copy.srcSubresource.baseArrayLayer = srcSubres.baseArrayLayer;
			copy.srcSubresource.layerCount = srcSubres.layerCount;
			copy.srcSubresource.mipLevel = srcSubres.baseMipLevel + m;

			copy.dstSubresource.aspectMask = dst.aspectMask;
			copy.dstSubresource.baseArrayLayer = 0u;
			copy.dstSubresource.layerCount = srcSubres.layerCount;
			copy.dstSubresource.mipLevel = m;

			extent.width = std::max(extent.width >> 1u, 1u);
			extent.height = std::max(extent.height >> 1u, 1u);
			extent.depth = std::max(extent.depth >> 1u, 1u);
		}

		dev.dispatch.CmdCopyImage(cb,
			src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			u32(copies.size()), copies.data());
	} else {
		auto resolves = memScope.alloc<VkImageResolve>(srcSubres.levelCount);
		for(auto m = 0u; m < srcSubres.levelCount; ++m) {
			auto& resolve = resolves[m];
			resolve.dstOffset = {};
			resolve.srcOffset = {};
			resolve.extent = extent;
			resolve.srcSubresource.aspectMask = srcSubres.aspectMask;
			resolve.srcSubresource.baseArrayLayer = srcSubres.baseArrayLayer;
			resolve.srcSubresource.layerCount = srcSubres.layerCount;
			resolve.srcSubresource.mipLevel = srcSubres.baseMipLevel + m;

			resolve.dstSubresource.aspectMask = dst.aspectMask;
			resolve.dstSubresource.baseArrayLayer = 0u;
			resolve.dstSubresource.layerCount = srcSubres.layerCount;
			resolve.dstSubresource.mipLevel = m;

			extent.width = std::max(extent.width >> 1u, 1u);
			extent.height = std::max(extent.height >> 1u, 1u);
			extent.depth = std::max(extent.depth >> 1u, 1u);
		}

		dev.dispatch.CmdResolveImage(cb,
			src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			u32(resolves.size()), resolves.data());
	}

	srcBarrier.oldLayout = srcBarrier.newLayout;
	srcBarrier.newLayout = srcLayout;
	srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	srcBarrier.dstAccessMask =
		VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT; // dunno

	dstBarrier.oldLayout = dstBarrier.newLayout;
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dunno, NOTE: probably could know
		0, 0, nullptr, 0, nullptr, 2, imgBarriers);
}

// Initializes the given OwnBuffer 'dst' and add commands to 'cb' (associated
// with queue family 'srcQueueFam') to copy the given 'srcSubres' from
// 'src' (which is in the given 'srcLayout') into 'dst'.
// After executing the recorded commands, 'dst' will contain srcSubres from src,
// as if sampled and converted to vec4 (i.e. rgba32f). Requires that 'src'
// supports nearest sampling. Will bind compute state to 'cb'.
// Only a single aspect must be set int srcSubres.
void initAndSampleCopy(Device& dev, VkCommandBuffer cb,
		CopiedImageToBuffer& dst, Image& src, VkImageLayout srcLayout,
		const VkImageSubresourceRange& srcSubres, u32 queueFamsBitset,
		std::vector<VkImageView>& imgViews, std::vector<VkBufferView>& bufViews,
		std::vector<VkDescriptorSet>& dss) {
	auto& hook = *dev.commandHook;
	dlg_assert(src.allowsNearestSampling);

	// determine the path we take here depending on the supported
	// format features
	enum class CopyMethod {
		copy,
		// TODO: implement! next best after copy
		//   but mainly relevant for cases where we don't have
		//   shaderStorageImageWriteWithoutFormat.
		//   And we want to support the texelStorage path in any case,
		//   for formats that can be sampled but don't have blitSrc
		// blit,
		texelStorage,
	};

	auto dstFormat = sampleFormat(src.ci.format,
		VkImageAspectFlagBits(srcSubres.aspectMask));

	CopyMethod method = CopyMethod::texelStorage;

	if(dstFormat == src.ci.format) {
		// TODO: cache somewhere, don't call this every time
		VkFormatProperties formatProps {};
		dev.ini->dispatch.GetPhysicalDeviceFormatProperties(dev.phdev,
			src.ci.format, &formatProps);
		auto checkTransfer = dev.props.apiVersion >= VK_API_VERSION_1_1;
		auto srcFeatures = (src.ci.tiling == VK_IMAGE_TILING_LINEAR) ?
			formatProps.linearTilingFeatures :
			formatProps.optimalTilingFeatures;

		if(!checkTransfer || (srcFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
			method = CopyMethod::copy;
		}
	}

	if(method != CopyMethod::copy) {
		if(dev.shaderStorageImageWriteWithoutFormat) {
			// make sure we can write dstFormat without format
			// TODO: cache somewhere, don't call this every time
			VkFormatProperties formatProps {};
			dev.ini->dispatch.GetPhysicalDeviceFormatProperties(dev.phdev,
				dstFormat, &formatProps);
			auto dstFeatures = formatProps.bufferFeatures;
			// TODO: is this enough? do we need VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT?
			// the vulkan spec gives some guarantees but only for
			// storage images, does it apply to storage texel buffers too?
			if(!(dstFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT)) {
				dstFormat = upgradeToSafeStorageTexelWithoutFormat(dstFormat);
			}
		} else {
			// our default format, see sample.glsl
			if(FormatIsSampledFloat(src.ci.format)) {
				dstFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
			} else if(FormatIsSINT(src.ci.format)) {
				dstFormat = VK_FORMAT_R32G32B32A32_SINT;
			} else if(FormatIsUINT(src.ci.format)) {
				dstFormat = VK_FORMAT_R32G32B32A32_UINT;
			}
		}
	}

	// = init buffer =
	dlg_assert(!FormatIsCompressed(dstFormat));
	dlg_assert(!FormatIsMultiplane(dstFormat));
	dlg_assert(FormatIsColor(dstFormat));
	dlg_assert(
		FormatIsSampledFloat(dstFormat) == FormatIsSampledFloat(src.ci.format) &&
		FormatIsSINT(dstFormat) == FormatIsSINT(src.ci.format) &&
		FormatIsUINT(dstFormat) == FormatIsUINT(src.ci.format));

	const auto texelSize = FormatTexelSize(dstFormat);

	auto texelCount = 0u;
	for(auto l = 0u; l < srcSubres.levelCount; ++l) {
		auto level = srcSubres.baseMipLevel + l;
		auto width = std::max(1u, src.ci.extent.width >> level);
		auto height = std::max(1u, src.ci.extent.height >> level);
		auto depth = std::max(1u, src.ci.extent.depth >> level);
		auto numTexelsPerLayer = width * height * depth;
		texelCount += numTexelsPerLayer * srcSubres.layerCount;
	}

	auto usage = VkBufferUsageFlags(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	auto opStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	auto opSrcAccess = VK_ACCESS_TRANSFER_READ_BIT;
	auto transitionTo = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	if(method == CopyMethod::texelStorage) {
		usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
		opStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		opSrcAccess = VK_ACCESS_SHADER_READ_BIT;
		transitionTo = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	} else {
		usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	}

	const auto neededSize = texelCount * texelSize;
	dst.buffer.ensure(dev, neededSize, usage, queueFamsBitset);
	dst.format = dstFormat;

	// = record =
	VkImageMemoryBarrier srcBarrier {};
	srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	srcBarrier.image = src.handle;
	srcBarrier.oldLayout = srcLayout;
	srcBarrier.newLayout = transitionTo;
	srcBarrier.srcAccessMask =
		VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
		VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	srcBarrier.dstAccessMask = opSrcAccess;
	srcBarrier.subresourceRange = srcSubres;
	srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dunno, NOTE: probably could
		opStage,
		0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

	if(method == CopyMethod::copy) {
		dlg_assert(src.ci.format == dstFormat);

		ThreadMemScope tms;
		auto copies = tms.alloc<VkBufferImageCopy>(srcSubres.levelCount);

		auto dstOffset = 0u;
		for(auto l = 0u; l < srcSubres.levelCount; ++l) {
			auto level = srcSubres.baseMipLevel + l;
			auto width = std::max(1u, src.ci.extent.width >> level);
			auto height = std::max(1u, src.ci.extent.height >> level);
			auto depth = std::max(1u, src.ci.extent.depth >> level);
			auto layerSize = width * height * depth;

			copies[l].imageExtent = {width, height, depth};
			copies[l].imageOffset = {0, 0, 0};
			copies[l].bufferOffset = dstOffset;
			copies[l].imageSubresource.aspectMask = srcSubres.aspectMask;
			copies[l].imageSubresource.baseArrayLayer = srcSubres.baseArrayLayer;
			copies[l].imageSubresource.layerCount = srcSubres.layerCount;
			copies[l].imageSubresource.mipLevel = level;

			dstOffset += texelSize * layerSize * srcSubres.layerCount;
		}

		dev.dispatch.CmdCopyImageToBuffer(cb, src.handle,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buffer.buf,
			u32(copies.size()), copies.data());
	} else {
		// create image view
		VkImageViewCreateInfo ivi {};
		ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivi.image = src.handle;
		ivi.viewType = imageViewForImageType(src.ci.imageType);
		ivi.format = src.ci.format;
		ivi.subresourceRange = srcSubres;

		auto& imgView = imgViews.emplace_back();
		VK_CHECK_DEV(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &imgView), dev);
		nameHandle(dev, imgView, "CommandHook:copyImage");

		// create/update descriptor bindings
		auto& ds = dss.emplace_back();
		VkDescriptorSetAllocateInfo dai {};
		dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dai.descriptorSetCount = 1u;
		dai.pSetLayouts = &hook.sampleImageDsLayout_;
		dai.descriptorPool = dev.dsPool;
		VK_CHECK_DEV(dev.dispatch.AllocateDescriptorSets(dev.handle, &dai, &ds), dev);

		VkDescriptorImageInfo imgInfo {};
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imgInfo.imageView = imgView;

		VkWriteDescriptorSet writes[2] {};
		VkBufferViewCreateInfo bvi {};
		bvi.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
		bvi.buffer = dst.buffer.buf;
		bvi.offset = 0u;
		bvi.range = dst.buffer.size;
		bvi.format = dstFormat;

		VkBufferView bufferView;
		VK_CHECK_DEV(dev.dispatch.CreateBufferView(dev.handle,
			&bvi, nullptr, &bufferView), dev);
		bufViews.push_back(bufferView);

		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].descriptorCount = 1u;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
		writes[0].pTexelBufferView = &bufViews.back();
		writes[0].dstBinding = 0u;
		writes[0].dstSet = ds;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].descriptorCount = 1u;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[1].pImageInfo = &imgInfo;
		writes[1].dstBinding = 1u;
		writes[1].dstSet = ds;

		dev.dispatch.UpdateDescriptorSets(dev.handle, 2u, writes, 0u, nullptr);
		dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			hook.sampleImagePipeLayout_, 0u, 1u, &ds, 0u, nullptr);

		auto sit = ShaderImageType::parseType(src.ci.imageType,
			src.ci.format, VkImageAspectFlagBits(srcSubres.aspectMask));
		dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			hook.sampleImagePipes_[sit]);

		auto dstOffset = 0u;
		for(auto l = 0u; l < srcSubres.levelCount; ++l) {
			auto level = srcSubres.baseMipLevel + l;
			auto width = std::max(1u, src.ci.extent.width >> level);
			auto height = std::max(1u, src.ci.extent.height >> level);
			auto depth = std::max(1u, src.ci.extent.depth >> level);
			auto layerSize = width * height * depth;

			struct {
				i32 level;
				u32 dstOffset;
			} pcr {
				i32(level),
				dstOffset,
			};

			dev.dispatch.CmdPushConstants(cb, hook.sampleImagePipeLayout_,
				VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(pcr), &pcr);

			auto groupSizeX = 8u;
			auto groupSizeY = 8u;
			auto groupSizeZ = 1u;
			auto z = depth;

			if(src.ci.imageType == VK_IMAGE_TYPE_1D) {
				groupSizeX = 64u;
				groupSizeY = 1u;
			}

			if(srcSubres.layerCount > 1u) {
				// 3D images can't have multiple layers
				z = srcSubres.layerCount;
			}

			auto gx = ceilDivide(width, groupSizeX);
			auto gy = ceilDivide(height, groupSizeY);
			auto gz = ceilDivide(z, groupSizeZ);

			dev.dispatch.CmdDispatch(cb, gx, gy, gz);

			dstOffset += layerSize * srcSubres.layerCount;
		}
	}

	// restore image state
	std::swap(srcBarrier.oldLayout, srcBarrier.newLayout);
	std::swap(srcBarrier.srcAccessMask, srcBarrier.dstAccessMask);

	dev.dispatch.CmdPipelineBarrier(cb,
		opStage,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dunno, NOTE: probably could
		0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
}

void performCopy(Device& dev, VkCommandBuffer cb, const Buffer& src,
		VkDeviceSize srcOffset, OwnBuffer& dst, VkDeviceSize dstOffset,
		VkDeviceSize size) {
	dlg_assert(dstOffset + size <= dst.size);
	dlg_assert(srcOffset + size <= src.ci.size);

	// perform copy
	VkBufferCopy copy {};
	copy.srcOffset = srcOffset;
	copy.dstOffset = dstOffset;
	copy.size = size;

	VkBufferMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.buffer = src.handle;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	barrier.size = copy.size;
	barrier.offset = copy.srcOffset;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 1, &barrier, 0, nullptr);

	dev.dispatch.CmdCopyBuffer(cb, src.handle, dst.buf, 1, &copy);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void initAndCopy(Device& dev, VkCommandBuffer cb, OwnBuffer& dst,
		VkBufferUsageFlags addFlags, Buffer& src,
		VkDeviceSize offset, VkDeviceSize size, u32 queueFamsBitset) {
	addFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	dst.ensure(dev, size, addFlags, queueFamsBitset);
	performCopy(dev, cb, src, offset, dst, 0, size);
}

void performCopy(Device& dev, VkCommandBuffer cb, VkDeviceAddress srcPtr,
		OwnBuffer& dst, VkDeviceSize dstOffset, VkDeviceSize size) {
	if(size == 0u) {
		return;
	}

	auto& srcBuf = bufferAtLocked(dev, srcPtr);
	dlg_assert(srcBuf.deviceAddress);
	auto srcOff = srcPtr - srcBuf.deviceAddress;
	performCopy(dev, cb, srcBuf, srcOff, dst, dstOffset, size);
}

void initAndCopy(Device& dev, VkCommandBuffer cb, OwnBuffer& dst,
		VkDeviceAddress srcPtr, VkDeviceSize size, u32 queueFamsBitset) {
	auto addFlags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	dst.ensure(dev, size, addFlags, queueFamsBitset);
	performCopy(dev, cb, srcPtr, dst, 0, size);
}

} // namespace vil
