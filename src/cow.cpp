#include <cow.hpp>
#include <device.hpp>
#include <image.hpp>
#include <layer.hpp>
#include <buffer.hpp>
#include <threadContext.hpp>
#include <util/debugMutex.hpp>
#include <gui/commandHook.hpp> // TODO: only for pipes
#include <vk/enumString.hpp>

namespace vil {

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
			vk::flagNames(VkImageUsageFlagBits(ici.usage)),
			vk::flagNames(VkImageCreateFlagBits(ici.flags)));
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
	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &image));
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
	auto memBits = memReqs.memoryTypeBits & dev.deviceLocalMemTypeBits;
	allocInfo.memoryTypeIndex = findLSB(memBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &memory));
	nameHandle(dev, this->memory, "CopiedImage:memory");

	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, image, memory, 0));

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
		VkImageLayout srcLayout, const VkImageSubresourceRange& srcSubres,
		u32 srcQueueFam) {
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

	auto layerCount = srcSubres.layerCount;
	auto levelCount = srcSubres.levelCount;

	if(layerCount == VK_REMAINING_ARRAY_LAYERS) {
		layerCount = src.ci.arrayLayers - srcSubres.baseArrayLayer;
	}
	if(levelCount == VK_REMAINING_MIP_LEVELS) {
		levelCount = src.ci.mipLevels - srcSubres.levelCount;
	}

	if(layerCount == 0u || levelCount == 0u ||
			extent.width == 0u || extent.height == 0u || extent.depth == 0u) {
		dlg_warn("Image copy would be empty");
		return;
	}

	auto success = dst.init(dev, src.ci.format, extent, layerCount,
		levelCount, srcSubres.aspectMask, srcQueueFam);
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
		VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
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
		VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
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
		OwnBuffer& dst, Image& src, VkImageLayout srcLayout,
		const VkImageSubresourceRange& srcSubres, u32 queueFamsBitset,
		std::vector<VkImageView>& imgViews, std::vector<VkDescriptorSet>& dss) {
	auto& hook = *dev.commandHook;
	dlg_assert(src.allowsNearestSampling);

	// = init buffer =
	// TODO PERF: only copy in the precision that we need. I.e. add
	// shader permutations that pack the data into 8/16/32 bit
	auto texelSize = sizeof(Vec4f);

	auto neededSize = 0u;
	for(auto l = 0u; l < srcSubres.levelCount; ++l) {
		auto level = srcSubres.baseMipLevel + l;
		auto width = std::max(1u, src.ci.extent.width >> level);
		auto height = std::max(1u, src.ci.extent.height >> level);
		auto depth = std::max(1u, src.ci.extent.depth >> level);
		auto layerSize = texelSize * width * height * depth;
		neededSize += layerSize * srcSubres.layerCount;
	}

	auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	dst.ensure(dev, neededSize, usage, queueFamsBitset);

	// = record =
	VkImageMemoryBarrier srcBarrier {};
	srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	srcBarrier.image = src.handle;
	srcBarrier.oldLayout = srcLayout;
	srcBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	srcBarrier.srcAccessMask =
		VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
		VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	srcBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	srcBarrier.subresourceRange = srcSubres;
	srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dunno, NOTE: probably could
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

	// create image view
	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = src.handle;
	ivi.viewType = imageViewForImageType(src.ci.imageType);
	ivi.format = src.ci.format;
	ivi.subresourceRange = srcSubres;

	auto& imgView = imgViews.emplace_back();
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &imgView));
	nameHandle(dev, imgView, "CommandHook:copyImage");

	// create/update descriptor bindings
	auto& ds = dss.emplace_back();
	VkDescriptorSetAllocateInfo dai {};
	dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dai.descriptorSetCount = 1u;
	dai.pSetLayouts = &hook.copyImageDsLayout_;
	dai.descriptorPool = dev.dsPool;
	VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dai, &ds));

	VkDescriptorImageInfo imgInfo {};
	imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgInfo.imageView = imgView;

	VkDescriptorBufferInfo bufInfo {};
	bufInfo.buffer = dst.buf;
	bufInfo.offset = 0u;
	bufInfo.range = dst.size;

	VkWriteDescriptorSet writes[2] {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].descriptorCount = 1u;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &bufInfo;
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
		hook.copyImagePipeLayout_, 0u, 1u, &ds, 0u, nullptr);

	auto sit = ShaderImageType::parseType(src.ci.imageType,
		src.ci.format, VkImageAspectFlagBits(srcSubres.aspectMask));
	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
		hook.copyImagePipes_[sit]);

	auto dstOffset = 0u;
	for(auto l = 0u; l < srcSubres.levelCount; ++l) {
		auto level = srcSubres.baseMipLevel + l;
		auto width = std::max(1u, src.ci.extent.width >> level);
		auto height = std::max(1u, src.ci.extent.height >> level);
		auto depth = std::max(1u, src.ci.extent.depth >> level);
		auto layerSize = texelSize * width * height * depth;

		struct {
			i32 level;
			u32 dstOffset;
		} pcr {
			i32(level),
			dstOffset,
		};

		dev.dispatch.CmdPushConstants(cb, hook.copyImagePipeLayout_,
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

	// restore image state
	std::swap(srcBarrier.oldLayout, srcBarrier.newLayout);
	std::swap(srcBarrier.srcAccessMask, srcBarrier.dstAccessMask);

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 1, &barrier, 0, nullptr);

	dev.dispatch.CmdCopyBuffer(cb, src.handle, dst.buf, 1, &copy);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
		0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void initAndCopy(Device& dev, VkCommandBuffer cb, OwnBuffer& dst,
		VkBufferUsageFlags addFlags, Buffer& src,
		VkDeviceSize offset, VkDeviceSize size, u32 queueFamsBitset) {
	addFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	dst.ensure(dev, size, addFlags, queueFamsBitset);
	performCopy(dev, cb, src, offset, dst, 0, size);
}

void recordResolve(Device& dev, CowResolveOp& op, CowBufferRange& cow) {
	dlg_assert(op.cb);
	dlg_assert(!cow.copy);
	dlg_assert(cow.source);
	dlg_assert(cow.offset + cow.size <= cow.source->ci.size);

	auto& dst = cow.copy.emplace();
	dst.op = &op;
	initAndCopy(dev, op.cb, dst.buf, cow.addFlags, *cow.source,
		cow.offset, cow.size, cow.queueFams);

	// we can unregister the cow and remove the reference to the
	// source now that it was resolved.
	assertOwned(dev.mutex);
	auto it = find(cow.source->cows, &cow);
	dlg_assert(it != cow.source->cows.end());
	cow.source->cows.erase(it);

	cow.source = nullptr;
	cow.offset = {};
	cow.size = {};
}

void recordResolve(Device& dev, CowResolveOp& op, CowImageRange& cow) {
	dlg_assert(op.cb);
	dlg_assert(cow.copy.index() == 0u);
	dlg_assert(cow.source);

	// TODO: might be problematic. Not sure we can always rely on
	// previous submissions (that actually realize this pending layout)
	// having finishing.
	auto layout = cow.source->pendingLayout;

	if(cow.imageAsBuffer) {
		auto& dst = cow.copy.emplace<BufferRangeCopy>();
		dst.op = &op;
		initAndSampleCopy(dev, op.cb, dst.buf, *cow.source,
			layout, cow.range, cow.queueFams, op.imageViews, op.descriptorSets);
	} else {
		auto& dst = cow.copy.emplace<ImageRangeCopy>();
		dst.op = &op;
		initAndCopy(dev, op.cb, dst.img, *cow.source,
			layout, cow.range, cow.queueFams);
	}

	// we can unregister the cow and remove the reference to the
	// source now that it was resolved.
	assertOwned(dev.mutex);
	auto it = find(cow.source->cows, &cow);
	dlg_assert(it != cow.source->cows.end());
	cow.source->cows.erase(it);

	cow.source = nullptr;
	cow.range = {};
}

CowImageRange::~CowImageRange() {
	std::lock_guard lock(source->dev->mutex);

	if(source) {
		dlg_assert(copy.index() == 0u);

		// unregister
		auto it = find(source->cows, this);
		dlg_assert(it != source->cows.end());
		source->cows.erase(it);
	}

	// check if operation is pending
	// we have to wait for it since our resource is being destroyed
	if(!source) {
		CowResolveOp* op {};
		Device* dev {};

		if(auto* buf = std::get_if<BufferRangeCopy>(&copy); buf) {
			op = buf->op;
			dev = buf->buf.dev;
		} else if(auto* img = std::get_if<ImageRangeCopy>(&copy); img) {
			op = img->op;
			dev = img->img.dev;
		}

		if(op) {
			dlg_assert(op->fence);
			dev->dispatch.WaitForFences(dev->handle, 1u, &op->fence, true, UINT64_MAX);
		}
	}
}

CowBufferRange::~CowBufferRange() {
	std::lock_guard lock(source->dev->mutex);

	if(source) {
		dlg_assert(!copy);

		// unregister
		auto it = find(source->cows, this);
		dlg_assert(it != source->cows.end());
		source->cows.erase(it);
	}

	// check if operation is pending
	// we have to wait for it since our resource is being destroyed
	if(!source && copy) {
		auto& dev = *copy->buf.dev;
		dlg_assert(copy->op->fence);
		dev.dispatch.WaitForFences(dev.handle, 1u, &copy->op->fence, true, UINT64_MAX);
	}
}

bool allowCow(const Image& img) {
	// we don't have proper support for tracking the memory of
	// sparse bindings yet
	if(img.ci.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
		return false;
	}

	// we can't track access to external resources
	if(img.externalMemory) {
		return false;
	}

	dlg_assert(img.memory);
	if(currentlyMappedLocked(img) || aliasesOtherResourceLocked(img)) {
		return false;
	}

	return true;
}

bool allowCowLocked(const Buffer& buf) {
	// we don't have proper support for tracking the memory of
	// sparse bindings yet
	if(buf.ci.flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
		return false;
	}

	// we can't track access to external resources
	if(buf.externalMemory) {
		return false;
	}

	dlg_assert(buf.memory);
	if(currentlyMappedLocked(buf) || aliasesOtherResourceLocked(buf)) {
		return false;
	}

	// If the buffer has a device address we have no chance at all
	// to track when it is being written
	if(buf.deviceAddress) {
		return false;
	}

	return true;
}

} // namespace vil
