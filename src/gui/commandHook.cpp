#include <gui/commandHook.hpp>
#include <commandDesc.hpp>
#include <image.hpp>
#include <rp.hpp>
#include <util.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <commands.hpp>
#include <device.hpp>

namespace fuen {

// util
ViewableImageCopy::ViewableImageCopy(Device& dev, VkFormat format, u32 width, u32 height) {
	this->dev = &dev;
	this->width = width;
	this->height = height;

	// dlg_trace("Creating image copy {} {} {}", this, width, height);

	// TODO: copy multiple layers?
	// TODO: support multisampling
	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.arrayLayers = 1u;
	ici.extent = {width, height, 1u};
	ici.format = format;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.mipLevels = 1;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &image));
	nameHandle(dev, this->image, "ViewableImageCopy:image");

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
	nameHandle(dev, this->memory, "ViewableImageCopy:memory");

	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, image, memory, 0));

	VkImageViewCreateInfo vci {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	vci.format = format;
	vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vci.subresourceRange.layerCount = 1u;
	vci.subresourceRange.levelCount = 1u;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &vci, nullptr, &imageView));
	nameHandle(dev, this->imageView, "ViewableImageCopy:imageView");
}

ViewableImageCopy::~ViewableImageCopy() {
	if(!dev) {
		return;
	}

	// dlg_trace("Destroying image copy {} {} {}", this, width, height);

	dev->dispatch.DestroyImageView(dev->handle, imageView, nullptr);
	dev->dispatch.DestroyImage(dev->handle, image, nullptr);
	dev->dispatch.FreeMemory(dev->handle, memory, nullptr);
}

// CommandHookImpl
VkCommandBuffer CommandHookImpl::hook(CommandBuffer& hooked,
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
		auto* our = dynamic_cast<CommandHookRecordImpl*>(record->hook.get());

		if(our && our->hook == this && our->hookCounter == counter_) {
			// In this case there is already a pending submission for this
			// record (can happen for simulataneous command buffers).
			// This is a problem since we can't write (and then, when
			// the submission finishes: read) the pool from multiple
			// places. We simply return the original cb in that case,
			// there is a pending submission querying that information after all.
			// NOTE: alternatively, we could create and store a new Record
			if(our->submissionCount != 0) {
				dlg_assert(our->submissionCount == 1);
				return hooked.handle();
			}

			data.reset(new CommandHookSubmissionImpl(*our, subm));
			++our->submissionCount;
			return our->cb;
		}
	}

	auto hook = new CommandHookRecordImpl(*this, *record, std::move(hcommand));
	record->hook.reset(hook);

	++hook->submissionCount;
	data.reset(new CommandHookSubmissionImpl(*hook, subm));

	return hook->cb;
}

void CommandHookImpl::desc(std::vector<CommandDesc> desc) {
	desc_ = std::move(desc);
	invalidateRecordings();
	invalidateData();
}

void CommandHookImpl::invalidateRecordings() {
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

CommandHookImpl::~CommandHookImpl() {
	invalidateRecordings();
}

// record
CommandHookRecordImpl::CommandHookRecordImpl(CommandHookImpl& xhook,
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
	nameHandle(dev, this->cb, "CommandHookRecordImpl:cb");

	// query pool
	VkQueryPoolCreateInfo qci {};
	qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qci.queryCount = 2u;
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &this->queryPool));
	nameHandle(dev, this->queryPool, "CommandHookRecordImpl:queryPool");

	// dstBuffer
	auto dstSize = VkDeviceSize(0);
	auto* hookDst = hcommand.back();
	auto splitRenderPass = false;
	auto createViewImage = false;

	if(dynamic_cast<DrawCmd*>(hookDst)) {
		createViewImage = true;
	} else if(dynamic_cast<DrawIndexedCmd*>(hookDst)) {
		createViewImage = true;
	} else if(auto* cmd = dynamic_cast<DrawIndirectCmd*>(hookDst)) {
		VkDeviceSize stride = cmd->indexed ?
			sizeof(VkDrawIndexedIndirectCommand) :
			sizeof(VkDrawIndirectCommand);
		stride = cmd->stride ? cmd->stride : stride;
		dstSize = cmd->drawCount * stride;
		splitRenderPass = true;
		createViewImage = true;
	} else if(auto* cmd = dynamic_cast<DispatchIndirectCmd*>(hookDst)) {
		(void) cmd;
		dstSize = sizeof(VkDispatchIndirectCommand);
		splitRenderPass = true;
	} else if(auto* cmd = dynamic_cast<DrawIndirectCountCmd*>(hookDst)) {
		VkDeviceSize stride = cmd->indexed ?
			sizeof(VkDrawIndexedIndirectCommand) :
			sizeof(VkDrawIndirectCommand);
		stride = cmd->stride ? cmd->stride : stride;
		// we get the maximum copy size via maxDrawCount but also
		// via the remaining buffer size.
		auto remSize = cmd->buffer->ci.size - cmd->offset;
		dstSize = std::min(cmd->maxDrawCount * stride, remSize);
		dstSize += sizeof(u32); // for the count
		splitRenderPass = true;
		createViewImage = true;
	}

	splitRenderPass |= createViewImage;

	if(dstSize > 0) {
		VkBufferCreateInfo bci {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		bci.size = dstSize;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bci, nullptr, &dstBuffer));
		nameHandle(dev, this->dstBuffer, "CommandHookRecordImpl:dstBuffer");

		VkMemoryRequirements memReqs;
		dev.dispatch.GetBufferMemoryRequirements(dev.handle, dstBuffer, &memReqs);

		// new memory
		VkMemoryAllocateInfo allocInfo {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReqs.size;
		auto memBits = memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits;
		allocInfo.memoryTypeIndex = findLSB(memBits);
		VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &dstMemory));
		nameHandle(dev, this->dstMemory, "CommandHookRecordImpl:dstMemory");

		VK_CHECK(dev.dispatch.BindBufferMemory(dev.handle, dstBuffer, dstMemory, 0));

		// map memory
		VK_CHECK(dev.dispatch.MapMemory(dev.handle, dstMemory, 0, VK_WHOLE_SIZE, 0, &bufferMap));
	}

	RecordInfo info;
	if(splitRenderPass) {
		// find the command that starts the render pass
		auto found = false;
		for(auto* cmd : hcommand) {
			if(auto rpCmd = dynamic_cast<BeginRenderPassCmd*>(cmd)) {
				dlg_assert(found == false);
				found = true;
				auto& desc = *rpCmd->rp->desc;

				info.beginRenderPassCmd = rpCmd;
				info.hookedSubpass = rpCmd->subpassOfDescendant(*hcommand.back());
				dlg_assert(info.hookedSubpass != u32(-1));
				dlg_assert(info.hookedSubpass < desc.subpasses.size());

				if(!splittable(desc, info.hookedSubpass)) {
					splitRenderPass = false;
					dlg_trace("Can't split render pass");
					break;
				}

				auto [rpi0, rpi1, rpi2] = splitInterruptable(desc);
				rp0 = create(dev, rpi0);
				rp1 = create(dev, rpi1);
				rp2 = create(dev, rpi2);

				if(createViewImage) {
					auto& subpass = desc.subpasses[info.hookedSubpass];

					// TODO: allow display of all attachments, not just
					// the first color attachment
					if(!subpass.colorAttachmentCount) {
						createViewImage = false;
						dlg_debug("Can't create view image; no color attachment");
					} else {
						auto& fb = *rpCmd->fb;
						auto attID = subpass.pColorAttachments[0].attachment;
						auto& attDesc = desc.attachments[attID];
						auto& imageView = fb.attachments[attID];
						dlg_assert(imageView);
						auto* image = imageView->img;

						if(!image) {
							dlg_warn("ImageView has no associated image");
						} else if(attDesc.samples != VK_SAMPLE_COUNT_1_BIT) {
							// TODO: support multisampling via vkCmdResolveImage
							//   alternatively we could check if the image is
							//   resolved at the end of the subpass and then simply
							//   copy that.
							dlg_debug("Can't create view image for multisampled attachment");
						} else if(!image->hasTransferSrc) {
							// There are only very specific cases where this can happen,
							// we could work around some of them (e.g. transient
							// attachment images or swapchain images that don't
							// support transferSrc).
							dlg_debug("Can't create view image for multisampled attachment");
						} else {
							// TODO: this should definitely not be created for
							//   each recording! Use a pool or something.
							// TODO: only use render pass area as size?
							auto* vimg = new ViewableImageCopy(dev, attDesc.format,
								fb.width, fb.height);
							dstImage.reset(vimg);
						}
					}
				}
			}
		}

		dlg_assert(found);
	}

	// record
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	// NOTE: we can never submit the cb simulataneously anyways, see the
	// 'submissionCount' branch we take when finding an already existent
	// record.
	// TODO: depending on the active hooks, we might be able to this
	// cbbi.flags = record->usageFlags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	VK_CHECK(dev.dispatch.BeginCommandBuffer(this->cb, &cbbi));

	// initial cmd stuff
	dev.dispatch.CmdResetQueryPool(cb, queryPool, 0, 2);

	info.splitRenderPass = splitRenderPass;
	this->hookRecord(record->commands, info);

	VK_CHECK(dev.dispatch.EndCommandBuffer(this->cb));
}

CommandHookRecordImpl::~CommandHookRecordImpl() {
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
	dev.dispatch.FreeMemory(dev.handle, dstMemory, nullptr);
	dev.dispatch.DestroyBuffer(dev.handle, dstBuffer, nullptr);

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

// TODO: this function is way too long. Factor out into
// - timing-related functions
// - renderpass splitting functions
void CommandHookRecordImpl::hookRecord(Command* cmd, RecordInfo info) {
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
			}

			if(!skipRecord) {
				cmd->record(dev, this->cb);
			}

			auto parentCmd = dynamic_cast<const ParentCommand*>(cmd);
			dlg_assert(hookDst || (parentCmd && parentCmd->children()));

			if(parentCmd) {
				if(hookDst) {
					// timing 0
					auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
					dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
				}

				++nextInfo.nextHookLevel;
				hookRecord(parentCmd->children(), nextInfo);
			}

			if(hookDst) {
				if(!parentCmd) {
					// timing 0
					auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
					dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
				}

				// timing 1
				auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				dev.dispatch.CmdWriteTimestamp(this->cb, stage1, this->queryPool, 1);

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

void CommandHookRecordImpl::beforeDstOutsideRp(Command& bcmd, const RecordInfo&) {
	auto& dev = record->device();
	auto doBarrierCopy = [this, &dev](const VkBufferCopy& copy, VkBuffer buffer) {
		VkBufferMemoryBarrier barrier {};
		barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.buffer = buffer;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
		barrier.size = copy.size;
		barrier.offset = copy.srcOffset;

		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 1, &barrier, 0, nullptr);

		dev.dispatch.CmdCopyBuffer(cb, buffer, dstBuffer, 1, &copy);

		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
			0, 0, nullptr, 1, &barrier, 0, nullptr);
	};

	if(auto* cmd = dynamic_cast<DrawIndirectCmd*>(&bcmd)) {
		VkDeviceSize stride = cmd->indexed ?
			sizeof(VkDrawIndexedIndirectCommand) :
			sizeof(VkDrawIndirectCommand);
		stride = cmd->stride ? cmd->stride : stride;
		auto dstSize = cmd->drawCount * stride;

		VkBufferCopy copy;
		copy.srcOffset = cmd->offset;
		copy.dstOffset = 0u;
		copy.size = dstSize;

		doBarrierCopy(copy, cmd->buffer->handle);
	} else if(auto* cmd = dynamic_cast<DispatchIndirectCmd*>(&bcmd)) {
		VkBufferCopy copy;
		copy.srcOffset = cmd->offset;
		copy.dstOffset = 0u;
		copy.size = sizeof(VkDispatchIndirectCommand);
		doBarrierCopy(copy, cmd->buffer->handle);
	} else if(auto* cmd = dynamic_cast<DrawIndirectCountCmd*>(&bcmd)) {
		(void) cmd;
		dlg_error("not implemented");
	}
}

void CommandHookRecordImpl::afterDstOutsideRp(Command& cmd, const RecordInfo& info) {
	(void) cmd;

	auto& dev = record->device();
	if(dstImage) {
		dlg_assert(info.beginRenderPassCmd);
		auto& desc = *info.beginRenderPassCmd->rp->desc;

		auto& subpass = desc.subpasses[info.hookedSubpass];
		auto& fb = *info.beginRenderPassCmd->fb;
		auto attID = subpass.pColorAttachments[0].attachment;

		dlg_assert(attID < fb.attachments.size());
		auto& imgView = *fb.attachments[attID];
		dlg_assert(imgView.img);

		auto& srcImg = *imgView.img;

		VkImageMemoryBarrier imgBarriers[2] {};

		auto& srcBarrier = imgBarriers[0];
		srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		srcBarrier.image = srcImg.handle;
		srcBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; // layout between rp splits, see rp.cpp
		srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
		srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		srcBarrier.subresourceRange.layerCount = 1u;
		srcBarrier.subresourceRange.levelCount = 1u;
		srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

		auto& dstBarrier = imgBarriers[1];
		dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		dstBarrier.image = dstImage->image;
		dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard
		dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		dstBarrier.srcAccessMask = 0u;
		dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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
		copy.extent = {fb.width, fb.height, 1u};
		copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1u;
		copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.dstSubresource.layerCount = 1u;

		dev.dispatch.CmdCopyImage(cb,
			srcImg.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dstImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copy);

		srcBarrier.oldLayout = srcBarrier.newLayout;
		srcBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
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
}

void CommandHookRecordImpl::finish() noexcept {
	// NOTE: We don't do this since we can assume the record to remain
	// valid until all submissions are finished. We can assume it to
	// be valid throughout the entire lifetime of *this.
	// record = nullptr;

	if(submissionCount == 0) {
		delete this;
	}
}

// submission
CommandHookSubmissionImpl::CommandHookSubmissionImpl(CommandHookRecordImpl& rec, PendingSubmission& subm)
		: record(&rec) {

	if(rec.dstImage) {
		dlg_assert(!rec.dstImage->writer);
		rec.dstImage->writer = &subm;
	}
}

CommandHookSubmissionImpl::~CommandHookSubmissionImpl() {
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

	transmitTiming();
	transmitIndirect();

	if(record->dstImage) {
		dlg_assert(record->dstImage->writer); // TODO: check that it's this submission
		record->dstImage->writer = nullptr;
		record->hook->image = record->dstImage;
	}
}

void CommandHookSubmissionImpl::transmitTiming() {
	auto& dev = record->record->device();

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
	record->hook->lastTime = diff;
}

void CommandHookSubmissionImpl::transmitIndirect() {
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

} // namespace fuen
