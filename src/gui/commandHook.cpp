#include <gui/commandHook.hpp>
#include <device.hpp>
#include <commandDesc.hpp>
#include <ds.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <rp.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <commands.hpp>
#include <util/util.hpp>
#include <vk/format_utils.h>

namespace fuen {

// TODO
struct ImageCopyOp {
	unsigned layer {}; // the layer to be copied
	unsigned mip {}; // the mip level to be copied
	VkImageAspectFlagBits aspect {}; // the aspect to be copied

	// If this isn't nullopt, will read the content of the specified
	// texel into a buffer, to be read on cpu.
	std::optional<VkOffset3D> readTexel {};
};

// util
void CopiedImage::init(Device& dev, VkFormat format, const VkExtent3D& extent) {
	this->dev = &dev;
	this->extent = extent;

	// TODO: support stencil
	// TODO: support non-float images
	// TODO: check if format supports sampling (and transfer_dst!)
	//   otherwise we could blit to an equivalent format.
	//   Or at least fail.

	if(FormatIsColor(format)) {
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	} else if(FormatHasDepth(format)) {
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	} else {
		dlg_error("unimplemented");
	}

	// TODO: copy multiple layers?
	// TODO: support multisampling?
	// TODO: support non-2d images
	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.arrayLayers = 1u;
	ici.extent = extent;
	ici.format = format;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.mipLevels = 1;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &image));
	nameHandle(dev, this->image, "CopiedImage:image");

	VkMemoryRequirements memReqs;
	dev.dispatch.GetImageMemoryRequirements(dev.handle, image, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	// TODO: create on hostVisible memory for easier image viewing?
	//   revisit this when implenting texel-values-in-gui feature
	auto memBits = memReqs.memoryTypeBits & dev.deviceLocalMemTypeBits;
	allocInfo.memoryTypeIndex = findLSB(memBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &memory));
	nameHandle(dev, this->memory, "CopiedImage:memory");

	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, image, memory, 0));

	VkImageViewCreateInfo vci {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	vci.format = format;
	vci.subresourceRange.aspectMask = aspect;
	vci.subresourceRange.layerCount = 1u;
	vci.subresourceRange.levelCount = 1u;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &vci, nullptr, &imageView));
	nameHandle(dev, this->imageView, "CopiedImage:imageView");
}

CopiedImage::~CopiedImage() {
	if(!dev) {
		return;
	}

	// dlg_trace("Destroying image copy {} {} {}", this, width, height);

	dev->dispatch.DestroyImageView(dev->handle, imageView, nullptr);
	dev->dispatch.DestroyImage(dev->handle, image, nullptr);
	dev->dispatch.FreeMemory(dev->handle, memory, nullptr);
}

void CopiedBuffer::init(Device& dev, VkDeviceSize size) {
	this->buffer.ensure(dev, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	dev.dispatch.MapMemory(dev.handle, buffer.mem, 0, size, 0, &this->map);
	this->copy = std::make_unique<std::byte[]>(size);

	// NOTE: destructor for copied buffer is not needed as memory mapping
	// is implicitly unmapped when memory of buffer is destroyed.
}

void CopiedBuffer::cpuCopy() {
	if(!buffer.mem) {
		return;
	}

	// TODO: only invalidate when on non-coherent memory
	VkMappedMemoryRange range[1] {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = buffer.mem;
	range[0].size = VK_WHOLE_SIZE;
	VK_CHECK(buffer.dev->dispatch.InvalidateMappedMemoryRanges(buffer.dev->handle, 1, range));
	std::memcpy(copy.get(), map, buffer.size);
}

// CommandHook
VkCommandBuffer CommandHook::hook(CommandBuffer& hooked,
		PendingSubmission& subm,
		FinishPtr<CommandHookSubmission>& data) {
	dlg_assert(hooked.state() == CommandBuffer::State::executable);

	// TODO: only hook when there is something to do

	// Check if it already has a valid record associated
	auto* record = hooked.lastRecordLocked();
	auto hcommand = CommandDesc::findHierarchy(record->commands, desc_);
	if(hcommand.empty()) {
		dlg_warn("Can't hook cb, can't find hooked command");
		return hooked.handle();
	}

	if(record->hook) {
		auto* our = dynamic_cast<CommandHookRecord*>(record->hook.get());

		if(our && our->hook == this && our->hookCounter == counter_) {
			// In this case there is already a pending submission for this
			// record (can happen for simulataneous command buffers).
			// This is a problem since we can't write (and then, when
			// the submission finishes: read) the pool from multiple
			// places. We simply return the original cb in that case,
			// there is a pending submission querying that information after all.
			// NOTE: alternatively, we could create and store a new Record
			// NOTE: alternatively, we could add a semaphore chaining
			//   this submission to the previous one.
			if(our->submissionCount != 0) {
				dlg_assert(our->submissionCount == 1);
				return hooked.handle();
			}

			data.reset(new CommandHookSubmission(*our, subm));
			++our->submissionCount;
			return our->cb;
		}
	}

	auto hook = new CommandHookRecord(*this, *record, std::move(hcommand));
	record->hook.reset(hook);

	++hook->submissionCount;
	data.reset(new CommandHookSubmission(*hook, subm));

	return hook->cb;
}

void CommandHook::desc(std::vector<CommandDesc> desc) {
	desc_ = std::move(desc);
	unsetHookOps();
}

void CommandHook::invalidateRecordings() {
	// We have to increase the counter to invalidate all past recordings
	++counter_;

	// Destroy all past recordings as soon as possible
	// (they might be kept alive by pending submissions)
	auto* rec = records_;
	while(rec) {
		// important to store this before we potentially destroy rec.
		auto* next = rec->next;

		rec->hook = nullptr; // notify the record that it's no longer needed
		if(rec->record->hook.get() == rec) {
			// CommandRecord::Hook is a FinishPtr.
			// This will delete our record hook if there are no pending
			// submissions of it left. See CommandHookRecord::finish
			rec->record->hook.reset();
		}

		rec = next;
	}

	records_ = nullptr;
}

void CommandHook::unsetHookOps() {
	this->copyIndexBuffers = false;
	this->copyVertexBuffers = false;
	this->queryTime = false;
	this->copyIndirectCmd = false;
	this->copyAttachment = {};
	this->copyDS = {};
	this->pcr = {};
	invalidateRecordings();
	invalidateData();
}

CommandHook::~CommandHook() {
	invalidateRecordings();
}

// record
CommandHookRecord::CommandHookRecord(CommandHook& xhook,
	CommandRecord& xrecord, std::vector<Command*> hooked) :
		hook(&xhook), record(&xrecord), hcommand(std::move(hooked)) {

	dlg_assert(!hcommand.empty());
	// this->dev = &xrecord.device();

	this->next = hook->records_;
	if(hook->records_) {
		hook->records_->prev = this;
	}
	hook->records_ = this;

	hookCounter = hook->counter_;

	auto& dev = xrecord.device();

	VkCommandBufferAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = dev.queueFamilies[record->queueFamily].commandPool;
	allocInfo.commandBufferCount = 1;

	VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &allocInfo, &this->cb));
	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, this->cb);
	nameHandle(dev, this->cb, "CommandHookRecord:cb");

	// query pool
	if(hook->queryTime) {
		VkQueryPoolCreateInfo qci {};
		qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		qci.queryCount = 2u;
		qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &this->queryPool));
		nameHandle(dev, this->queryPool, "CommandHookRecord:queryPool");
	}

	RecordInfo info {};
	initState(info);

	// record
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	// we can never submit the cb simulataneously anyways, see the
	// 'submissionCount' branch we take when finding an already existent
	// record.
	// cbbi.flags = record->usageFlags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	VK_CHECK(dev.dispatch.BeginCommandBuffer(this->cb, &cbbi));

	// initial cmd stuff
	if(hook->queryTime) {
		dev.dispatch.CmdResetQueryPool(cb, queryPool, 0, 2);
	}

	this->hookRecord(record->commands, info);

	VK_CHECK(dev.dispatch.EndCommandBuffer(this->cb));
}

CommandHookRecord::~CommandHookRecord() {
	// We can be sure that record is still alive here since when the
	// record is destroyed, all its submissions must have finished as well.
	// And then we would have been destroyed via the finish() command (see
	// the assertions there)
	dlg_assert(record);
	dlg_assert(submissionCount == 0);

	auto& dev = record->device();

	// destroy resources
	auto commandPool = dev.queueFamilies[record->queueFamily].commandPool;

	dev.dispatch.FreeCommandBuffers(dev.handle, commandPool, 1, &cb);
	dev.dispatch.DestroyQueryPool(dev.handle, queryPool, nullptr);

	dev.dispatch.DestroyRenderPass(dev.handle, rp0, nullptr);
	dev.dispatch.DestroyRenderPass(dev.handle, rp1, nullptr);
	dev.dispatch.DestroyRenderPass(dev.handle, rp2, nullptr);

	// unlink
	if(next) {
		next->prev = prev;
	}
	if(prev) {
		prev->next = next;
	}
	if(hook && this == hook->records_) {
		dlg_assert(!prev);
		hook->records_ = next;
	}
}

void CommandHookRecord::initState(RecordInfo& info) {
	auto& dev = record->device();
	state.reset(new CommandHookState());

	// Find out if final hooked command is inside render pass
	auto preEnd = hcommand.end() - 1;
	for(auto it = hcommand.begin(); it != preEnd; ++it) {
		auto* cmd = *it;
		if(info.beginRenderPassCmd = dynamic_cast<const BeginRenderPassCmd*>(cmd); info.beginRenderPassCmd) {
			break;
		}
	}

	dlg_assert(info.beginRenderPassCmd ||
		(!hook->copyVertexBuffers && !hook->copyIndexBuffers && !hook->copyAttachment));

	info.splitRenderPass = info.beginRenderPassCmd &&
		(hook->copyVertexBuffers ||
		 hook->copyIndexBuffers ||
		 hook->copyAttachment ||
		 hook->copyDS ||
		 hook->copyIndirectCmd);

	if(info.splitRenderPass) {
		auto& rp = *info.beginRenderPassCmd->rp;

		// TODO: we could likely support this
		if(hasChain(*rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
			dlg_trace("Can't split multiview renderpass");
			info.splitRenderPass = false;
		}
	}

	if(info.splitRenderPass) {
		auto& desc = *info.beginRenderPassCmd->rp->desc;

		info.beginRenderPassCmd = info.beginRenderPassCmd;
		info.hookedSubpass = info.beginRenderPassCmd->subpassOfDescendant(*hcommand.back());
		dlg_assert(info.hookedSubpass != u32(-1));
		dlg_assert(info.hookedSubpass < desc.subpasses.size());

		if(!splittable(desc, info.hookedSubpass)) {
			info.splitRenderPass = false;
			dlg_trace("Can't split render pass");
		} else {
			auto [rpi0, rpi1, rpi2] = splitInterruptable(desc);
			rp0 = create(dev, rpi0);
			rp1 = create(dev, rpi1);
			rp2 = create(dev, rpi2);
		}
	}
}

// TODO: this function is way too long. Factor out into
// - timing-related functions
// - renderpass splitting functions
void CommandHookRecord::hookRecord(Command* cmd, RecordInfo info) {
	auto& dev = record->device();
	while(cmd) {
		auto nextInfo = info;

		// check if command is on hooking chain
		if(info.nextHookLevel < hcommand.size() && cmd == hcommand[info.nextHookLevel]) {
			auto hookDst = (info.nextHookLevel == hcommand.size() - 1);
			auto skipRecord = false;

			auto* beginRpCmd = dynamic_cast<BeginRenderPassCmd*>(cmd);
			if(info.splitRenderPass && beginRpCmd) {
				dlg_assert(rp0);
				auto rpBeginInfo = beginRpCmd->info;
				rpBeginInfo.renderPass = rp0;

				if(beginRpCmd->subpassBeginInfo.pNext) {
					auto beginRp2 = selectCmd(
						dev.dispatch.CmdBeginRenderPass2,
						dev.dispatch.CmdBeginRenderPass2KHR);
					dlg_assert(beginRp2);
					beginRp2(cb, &rpBeginInfo, &beginRpCmd->subpassBeginInfo);
				} else {
					dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo,
						beginRpCmd->subpassBeginInfo.contents);
				}

				// dlg_assert(!nextInfo.beginRenderPassCmd);
				// nextInfo.beginRenderPassCmd = beginRpCmd;
				dlg_assert(nextInfo.beginRenderPassCmd == beginRpCmd);
				skipRecord = true;
			}

			// before hook
			// TODO: also call beforeDstOutsideRp when there is no rp to split,
			// e.g. for dispatch or transfer commands.
			if(hookDst && info.splitRenderPass) {
				dlg_assert(info.beginRenderPassCmd);

				// TODO: missing potential forward of pNext chain here
				auto numSubpasses = info.beginRenderPassCmd->rp->desc->subpasses.size();
				for(auto i = info.hookedSubpass; i + 1 < numSubpasses; ++i) {
					// TODO: missing potential forward of pNext chain here
					// TODO: subpass contents relevant?
					dev.dispatch.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
				}
				dev.dispatch.CmdEndRenderPass(cb);

				beforeDstOutsideRp(*cmd, info);

				dlg_assert(rp1);
				auto rpBeginInfo = info.beginRenderPassCmd->info;
				rpBeginInfo.renderPass = rp1;
				// we don't clear anything when starting this rp
				rpBeginInfo.pClearValues = nullptr;
				rpBeginInfo.clearValueCount = 0u;

				if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
					auto beginRp2 = selectCmd(
						dev.dispatch.CmdBeginRenderPass2,
						dev.dispatch.CmdBeginRenderPass2KHR);
					dlg_assert(beginRp2);
					beginRp2(cb, &rpBeginInfo, &info.beginRenderPassCmd->subpassBeginInfo);
				} else {
					dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo,
						info.beginRenderPassCmd->subpassBeginInfo.contents);
				}

				for(auto i = 0u; i < info.hookedSubpass; ++i) {
					// TODO: missing potential forward of pNext chain here
					// TODO: subpass contents relevant?
					dev.dispatch.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
				}
			} else if(hookDst && !info.splitRenderPass && !info.beginRenderPassCmd) {
				beforeDstOutsideRp(*cmd, info);
			}

			if(!skipRecord) {
				cmd->record(dev, this->cb);
			}

			auto parentCmd = dynamic_cast<const ParentCommand*>(cmd);
			dlg_assert(hookDst || (parentCmd && parentCmd->children()));

			if(parentCmd) {
				if(hookDst && queryPool) {
					// timing 0
					auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
					dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
				}

				++nextInfo.nextHookLevel;
				hookRecord(parentCmd->children(), nextInfo);
			}

			if(hookDst) {
				if(queryPool) {
					if(!parentCmd) {
						// timing 0
						auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
						dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
					}

					// timing 1
					auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
					dev.dispatch.CmdWriteTimestamp(this->cb, stage1, this->queryPool, 1);
				}

				// render pass split: rp2
				if(info.splitRenderPass) {
					dlg_assert(info.beginRenderPassCmd);

					// TODO: missing potential forward of pNext chain here
					auto numSubpasses = info.beginRenderPassCmd->rp->desc->subpasses.size();
					for(auto i = info.hookedSubpass; i + 1 < numSubpasses; ++i) {
						// TODO: missing potential forward of pNext chain here
						// TODO: subpass contents relevant?
						dev.dispatch.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
					}
					dev.dispatch.CmdEndRenderPass(cb);

					afterDstOutsideRp(*cmd, info);

					dlg_assert(rp2);
					auto rpBeginInfo = info.beginRenderPassCmd->info;
					rpBeginInfo.renderPass = rp2;
					// we don't clear anything when starting this rp
					rpBeginInfo.pClearValues = nullptr;
					rpBeginInfo.clearValueCount = 0u;

					if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
						auto beginRp2 = selectCmd(
							dev.dispatch.CmdBeginRenderPass2,
							dev.dispatch.CmdBeginRenderPass2KHR);
						dlg_assert(beginRp2);
						beginRp2(cb, &rpBeginInfo, &info.beginRenderPassCmd->subpassBeginInfo);
					} else {
						dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo,
							info.beginRenderPassCmd->subpassBeginInfo.contents);
					}

					for(auto i = 0u; i < info.hookedSubpass; ++i) {
						// TODO: missing potential forward of pNext chain here
						// TODO: subpass contents relevant?
						dev.dispatch.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
					}
				} else if(!info.splitRenderPass && !info.beginRenderPassCmd) {
					afterDstOutsideRp(*cmd, info);
				}
			}
		} else {
			cmd->record(dev, this->cb);
			if(auto parentCmd = dynamic_cast<const ParentCommand*>(cmd); parentCmd) {
				hookRecord(parentCmd->children(), info);
			}
		}

		cmd = cmd->next;
	}
}

void initAndCopy(Device& dev, VkCommandBuffer cb, CopiedImage& dst, Image& src,
		VkImageLayout srcLayout, const VkImageSubresource& srcSubres) {
	if(src.ci.samples != VK_SAMPLE_COUNT_1_BIT) {
		// TODO: support multisampling via vkCmdResolveImage
		//   alternatively we could check if the image is
		//   resolved at the end of the subpass and then simply
		//   copy that.
		dlg_debug("Can't copy/display multisampled attachment");
		return;
	} else if(!src.hasTransferSrc) {
		// There are only very specific cases where this can happen,
		// we could work around some of them (e.g. transient
		// attachment images or swapchain images that don't
		// support transferSrc).
		dlg_debug("Can't display image copy; original can't be copied");
		return;
	}

	auto extent = src.ci.extent;
	for(auto i = 0u; i < srcSubres.mipLevel; ++i) {
		extent.width = std::max(extent.width >> 1, 1u);

		if(extent.height) {
			extent.height = std::max(extent.height >> 1, 1u);
		}

		if(extent.depth) {
			extent.depth = std::max(extent.depth >> 1, 1u);
		}
	}

	dst.init(dev, src.ci.format, extent);

	// perform copy
	VkImageMemoryBarrier imgBarriers[2] {};

	auto& srcBarrier = imgBarriers[0];
	srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	srcBarrier.image = src.handle;
	srcBarrier.oldLayout = srcLayout;
	srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	srcBarrier.subresourceRange.aspectMask = srcSubres.aspectMask;
	srcBarrier.subresourceRange.baseArrayLayer = srcSubres.arrayLayer;
	srcBarrier.subresourceRange.baseMipLevel = srcSubres.mipLevel;
	srcBarrier.subresourceRange.layerCount = 1u;
	srcBarrier.subresourceRange.levelCount = 1u;
	srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	auto& dstBarrier = imgBarriers[1];
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.image = dst.image;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcAccessMask = 0u;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.subresourceRange.aspectMask = dst.aspect;
	dstBarrier.subresourceRange.layerCount = 1u;
	dstBarrier.subresourceRange.levelCount = 1u;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno, NOTE: probably could
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 2, imgBarriers);

	// TODO: kinda hacky, can be improved. But we definitely need a general barrier here,
	// between the render passes.
	VkMemoryBarrier memBarrier {};
	memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
	memBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 1, &memBarrier, 0, nullptr, 0, nullptr);

	VkImageCopy copy {};
	copy.dstOffset = {};
	copy.srcOffset = {};
	copy.extent = extent;
	copy.srcSubresource.aspectMask = srcSubres.aspectMask;
	copy.srcSubresource.baseArrayLayer = srcSubres.arrayLayer;
	copy.srcSubresource.mipLevel = srcSubres.mipLevel;
	copy.srcSubresource.layerCount = 1u;

	copy.dstSubresource.aspectMask = dst.aspect;
	copy.dstSubresource.layerCount = 1u;

	dev.dispatch.CmdCopyImage(cb,
		src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &copy);

	srcBarrier.oldLayout = srcBarrier.newLayout;
	srcBarrier.newLayout = srcLayout;
	srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	srcBarrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno

	dstBarrier.oldLayout = dstBarrier.newLayout;
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dunno, NOTE: probably could know
		0, 0, nullptr, 0, nullptr, 2, imgBarriers);
}

void initAndCopy(Device& dev, VkCommandBuffer cb, CopiedBuffer& dst, VkBuffer src,
		VkDeviceSize offset, VkDeviceSize size) {
	// init dst
	dst.init(dev, size);

	// perform copy
	VkBufferCopy copy {};
	copy.srcOffset = offset;
	copy.dstOffset = 0u;
	copy.size = size;

	VkBufferMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.buffer = src;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	barrier.size = copy.size;
	barrier.offset = copy.srcOffset;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 1, &barrier, 0, nullptr);

	dev.dispatch.CmdCopyBuffer(cb, src, dst.buffer.buf, 1, &copy);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
		0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void CommandHookRecord::copyDs(Command& bcmd, const RecordInfo& info) {
	auto& dev = record->device();

	DescriptorState* dsState = nullptr;
	if(auto* drawCmd = dynamic_cast<DrawCmdBase*>(&bcmd)) {
		dsState = &drawCmd->state;
	} else if(auto* dispatchCmd = dynamic_cast<DispatchCmdBase*>(&bcmd)) {
		dsState = &dispatchCmd->state;
	} else {
		dlg_error("unsupported descriptor command");
	}

	if(!dsState) {
		return;
	}

	auto [setID, bindingID, elemID] = *hook->copyDS;

	// NOTE: we have to check for correct sizes here since the
	// actual command might have changed (for an updated record)
	// and the selected one not valid anymore.
	if(setID >= dsState->descriptorSets.size()) {
		dlg_trace("setID out of range");
		dsState->descriptorSets = {};
		return;
	}

	auto& set = dsState->descriptorSets[setID];

	if(bindingID >= nonNull(set.ds).bindings.size()) {
		dlg_trace("bindingID out of range");
		dsState->descriptorSets = {};
		return;
	}

	auto& binding = set.ds->bindings[bindingID];

	if(elemID >= binding.size()) {
		dlg_trace("elemID out of range");
		dsState->descriptorSets = {};
		return;
	}

	auto& elem = binding[elemID];

	dlg_assert(elem.valid);
	auto lbinding = set.ds->layout->bindings[bindingID];
	auto cat = category(lbinding.descriptorType);
	if(cat == DescriptorCategory::image) {
		if(needsImageView(lbinding.descriptorType)) {
			auto* imgView = elem.imageInfo.imageView;
			dlg_assert(imgView);
			dlg_assert(imgView->img);
			if(imgView->img) {
				auto& dst = state->dsCopy.emplace<CopiedImage>();

				// We have to handle the special case where a renderpass
				// attachment is bound in a descriptor set (e.g. as
				// input attachment). In that case, it will be
				// in general layout (via our render pass splitting),
				// not in the layout of the ds.
				auto layout = elem.imageInfo.layout;
				if(info.splitRenderPass) {
					auto& fb = nonNull(nonNull(info.beginRenderPassCmd).fb);
					for(auto* att : fb.attachments) {
						dlg_assert(att->img);
						if(att->img == imgView->img) {
							layout = VK_IMAGE_LAYOUT_GENERAL;
							break;
						}
					}
				}

				// TODO: select exact layer/mip in view range via gui
				VkImageSubresource subres {};
				subres.aspectMask = imgView->ci.subresourceRange.aspectMask;
				subres.arrayLayer = imgView->ci.subresourceRange.baseArrayLayer;
				subres.mipLevel = imgView->ci.subresourceRange.baseMipLevel;
				initAndCopy(dev, cb, dst, *imgView->img, layout, subres);
			}
		} else {
			// TODO: bad message, maybe just show a link to the sampler
			//   in the commands.cpp functions?
			dlg_warn("Nothing to copy: just a sampler bound");
		}
	} else if(cat == DescriptorCategory::buffer) {
		auto& dst = state->dsCopy.emplace<CopiedBuffer>();
		auto range = elem.bufferInfo.range;
		if(range == VK_WHOLE_SIZE) {
			range = elem.bufferInfo.buffer->ci.size - elem.bufferInfo.offset;
		}

		auto size = std::min(maxBufCopySize, range);
		initAndCopy(dev, cb, dst, elem.bufferInfo.buffer->handle,
			elem.bufferInfo.offset, size);
	} else if(cat == DescriptorCategory::bufferView) {
		// TODO: copy as buffer or image? maybe best to copy
		//   as buffer but then create bufferView on our own?
		// auto& dst = state->dsCopy.emplace<CopiedBuffer>();
		// dlg_assert(elem.bufferView->buffer);
		// copyBuffer(dst, elem.bufferView->buffer->handle,
		// 	elem.bufferView->ci.offset, elem.bufferView->ci.range);
		dlg_error("bufferview ds copy unimplemented");
	}
}

void CommandHookRecord::beforeDstOutsideRp(Command& bcmd, const RecordInfo& info) {
	auto& dev = record->device();
	DebugLabel lbl(dev, cb, "beforeDstOutsideRp");

	// indirect copy
	if(hook->copyIndirectCmd) {
		if(auto* cmd = dynamic_cast<DrawIndirectCmd*>(&bcmd)) {
			VkDeviceSize stride = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd->stride ? cmd->stride : stride;
			auto dstSize = cmd->drawCount * stride;
			initAndCopy(dev, cb, state->indirectCopy, cmd->buffer->handle, cmd->offset, dstSize);
		} else if(auto* cmd = dynamic_cast<DispatchIndirectCmd*>(&bcmd)) {
			auto size = sizeof(VkDispatchIndirectCommand);
			initAndCopy(dev, cb, state->indirectCopy, cmd->buffer->handle, cmd->offset, size);
		} else if(auto* cmd = dynamic_cast<DrawIndirectCountCmd*>(&bcmd)) {
			(void) cmd;
			dlg_error("not implemented");
		} else {
			dlg_error("unsupported indirect command");
		}
	}

	// descriptor state
	if(hook->copyDS) {
		copyDs(bcmd, info);
	}

	auto* drawCmd = dynamic_cast<DrawCmdBase*>(&bcmd);

	// TODO: for non-indirect, non-indexed commands we know the exact number
	// of indices to copy
	if(hook->copyVertexBuffers) {
		dlg_assert(drawCmd);
		for(auto& vertbuf : drawCmd->state.vertices) {
			auto& dst = state->vertexBufCopies.emplace_back();
			if(!vertbuf.buffer) {
				continue;
			}

			auto size = std::min(maxBufCopySize, vertbuf.buffer->ci.size - vertbuf.offset);
			initAndCopy(dev, cb, dst, vertbuf.buffer->handle, vertbuf.offset, size);
		}
	}

	// TODO: for non-indirect commands we know the exact number of indices to copy
	if(hook->copyIndexBuffers) {
		dlg_assert(drawCmd);
		auto& inds = drawCmd->state.indices;
		if(inds.buffer) {
			auto size = std::min(maxBufCopySize, inds.buffer->ci.size - inds.offset);
			initAndCopy(dev, cb, state->indexBufCopy, inds.buffer->handle, inds.offset, size);
		}
	}
}

void CommandHookRecord::afterDstOutsideRp(Command&, const RecordInfo& info) {
	auto& dev = record->device();
	DebugLabel lbl(dev, cb, "afterDsOutsideRp");

	if(hook->copyAttachment) {
		// initialize dst image
		dlg_assert(info.beginRenderPassCmd);
		auto& fb = nonNull(info.beginRenderPassCmd->fb);
		auto attID = *hook->copyAttachment;
		if(attID >= fb.attachments.size()) {
			hook->copyAttachment = {};
			dlg_trace("copyAttachment out of range");
		} else {
			auto& imageView = fb.attachments[attID];
			dlg_assert(imageView);
			dlg_assert(imageView->img);
			auto* image = imageView->img;

			if(!image) {
				dlg_warn("ImageView has no associated image");
			} else {
				auto& srcImg = *image;
				auto layout = VK_IMAGE_LAYOUT_GENERAL; // layout between rp splits, see rp.cpp

				// TODO: select exact layer/mip in view range via gui
				VkImageSubresource subres {};
				subres.aspectMask = imageView->ci.subresourceRange.aspectMask;
				subres.arrayLayer = imageView->ci.subresourceRange.baseArrayLayer;
				subres.mipLevel = imageView->ci.subresourceRange.baseMipLevel;
				initAndCopy(dev, cb, state->attachmentCopy, srcImg, layout, subres);
			}
		}
	}
}

void CommandHookRecord::finish() noexcept {
	// NOTE: We don't do this since we can assume the record to remain
	// valid until all submissions are finished. We can assume it to
	// be valid throughout the entire lifetime of *this.
	// record = nullptr;

	if(submissionCount == 0) {
		delete this;
	}
}

// submission
CommandHookSubmission::CommandHookSubmission(CommandHookRecord& rec, PendingSubmission&)
		: record(&rec) {
}

CommandHookSubmission::~CommandHookSubmission() {
	dlg_assert(record && record->record);

	// We must be the only pending submission.
	dlg_assert(record->submissionCount == 1u);
	--record->submissionCount;

	// In this case the hook was removed, no longer interested in results.
	// Since we are the only submission left to the record, it can be
	// destroyed.
	if(!record->hook) {
		delete record;
		return;
	}

	dlg_assert(record->state);
	if(record->state) {
		// transmitIndirect();
		transmitTiming();
		record->hook->state = record->state;

		// copy potentiall buffers
		// TODO: messy, ugly, shouldn't be here like this.
		if(auto* buf = std::get_if<CopiedBuffer>(&record->state->dsCopy)) {
			buf->cpuCopy();
		}
		record->state->indexBufCopy.cpuCopy();
		record->state->indirectCopy.cpuCopy();
		for(auto& vbuf : record->state->vertexBufCopies) {
			vbuf.cpuCopy();
		}
	}
}

void CommandHookSubmission::transmitTiming() {
	auto& dev = record->record->device();

	dlg_assert(bool(record->queryPool) == record->hook->queryTime);
	if(!record->queryPool || !record->hook->queryTime) {
		return;
	}

	// Store the query pool results.
	// Since the submission finished, we can expect them to be available
	// soon, so we wait for them.
	u64 data[2];
	auto res = dev.dispatch.GetQueryPoolResults(dev.handle, record->queryPool, 0, 2,
		sizeof(data), data, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	// check if query is available
	if(res != VK_SUCCESS) {
		dlg_error("GetQueryPoolResults failed: {}", res);
		return;
	}

	u64 before = data[0];
	u64 after = data[1];

	auto diff = after - before;
	record->hook->state->neededTime = diff;
}

/*
void CommandHookSubmission::transmitIndirect() {
	if(!record->bufferMap) {
		return;
	}

	auto& dev = record->record->device();

	// TODO: only call on non-coherent memory
	VkMappedMemoryRange range {};
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	range.memory = record->dstMemory;
	VK_CHECK(dev.dispatch.InvalidateMappedMemoryRanges(dev.handle, 1, &range));

	auto* hookDst = record->hcommand.back();
	auto& dstIndirect = record->hook->indirect;

	if(auto* cmd = dynamic_cast<DrawIndirectCmd*>(hookDst)) {
		VkDeviceSize cmdSize = cmd->indexed ?
			sizeof(VkDrawIndexedIndirectCommand) :
			sizeof(VkDrawIndirectCommand);
		auto stride = cmd->stride ? cmd->stride : cmdSize;

		dstIndirect.count = cmd->drawCount;
		dstIndirect.data.resize(cmd->drawCount * cmdSize);

		for(auto i = 0u; i < cmd->drawCount; ++i) {
			auto src = reinterpret_cast<std::byte*>(record->bufferMap) + i * stride;
			auto dst = dstIndirect.data.data() + i * cmdSize;
			std::memcpy(dst, src, cmdSize);
		}
	} else if(auto* cmd = dynamic_cast<DispatchIndirectCmd*>(hookDst)) {
		(void) cmd;
		auto size = sizeof(VkDispatchIndirectCommand);
		dstIndirect.count = 1u;
		dstIndirect.data.resize(size);
		std::memcpy(record->hook->indirect.data.data(), record->bufferMap, size);
	} else if(auto* cmd = dynamic_cast<DrawIndirectCountCmd*>(hookDst)) {
		(void) cmd;
		dlg_error("Not implemented");
	}
}
*/

} // namespace fuen
