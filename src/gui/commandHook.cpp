#include <gui/commandHook.hpp>
#include <device.hpp>
#include <ds.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <rp.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <command/desc.hpp>
#include <command/commands.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <vk/format_utils.h>

// TODO: instead of doing memory barrier per-resource when copying to
//   our readback buffers, we should probably do just do general memory
//   barriers.

namespace vil {

// util
void CopiedImage::init(Device& dev, VkFormat format, const VkExtent3D& extent,
		u32 layers, u32 levels, VkImageAspectFlags aspects, u32 srcQueueFam) {
	ZoneScoped;

	this->dev = &dev;
	this->extent = extent;
	this->levelCount = levels;
	this->layerCount = layers;
	this->aspectMask = aspects;
	this->format = format;

	// TODO: evaluate if the image can be used for everything we want
	//   to use it for. Could blit to related format if not.
	// TODO: support multisampling?
	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.arrayLayers = layerCount;
	ici.extent = extent;
	ici.format = format;
	ici.imageType = minImageType(this->extent);
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if(srcQueueFam == dev.gfxQueue->family) {
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	} else {
		// PERF: we could just perform an explicit transition in this case,
		//   it's really not hard here
		std::array<u32, 2> qfams = {dev.gfxQueue->family, srcQueueFam};
		ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
		ici.pQueueFamilyIndices = qfams.data();
		ici.queueFamilyIndexCount = u32(qfams.size());
	}

	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.mipLevels = levelCount;
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
	// NOTE: even though using host visible memory would make some operations
	//   eaiser (such as showing a specific texel value in gui), the guarantees
	//   vulkan gives for support of linear images are quite small.
	auto memBits = memReqs.memoryTypeBits & dev.deviceLocalMemTypeBits;
	allocInfo.memoryTypeIndex = findLSB(memBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &memory));
	nameHandle(dev, this->memory, "CopiedImage:memory");

	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, image, memory, 0));

	VkImageViewCreateInfo vci {};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = image;
	switch(ici.imageType) {
		case VK_IMAGE_TYPE_1D:
			vci.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			break;
		case VK_IMAGE_TYPE_2D:
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			break;
		case VK_IMAGE_TYPE_3D:
			vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
			break;
		default:
			dlg_error("unreachable");
			break;
	}
	vci.format = format;
	vci.subresourceRange.aspectMask = aspectMask & ~(VK_IMAGE_ASPECT_STENCIL_BIT);
	vci.subresourceRange.layerCount = layerCount;
	vci.subresourceRange.levelCount = levelCount;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &vci, nullptr, &imageView));
	nameHandle(dev, this->imageView, "CopiedImage:imageView");

	if(aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
		vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
		VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &vci, nullptr, &stencilView));
		nameHandle(dev, this->stencilView, "CopiedImage:stencilView");
	}
}

CopiedImage::~CopiedImage() {
	if(!dev) {
		return;
	}

	dev->dispatch.DestroyImageView(dev->handle, imageView, nullptr);
	dev->dispatch.DestroyImageView(dev->handle, stencilView, nullptr);
	dev->dispatch.DestroyImage(dev->handle, image, nullptr);
	dev->dispatch.FreeMemory(dev->handle, memory, nullptr);
}

void CopiedBuffer::init(Device& dev, VkDeviceSize size, VkBufferUsageFlags addFlags) {
	auto usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | addFlags;

	this->buffer.ensure(dev, size, usage);
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, buffer.mem, 0, VK_WHOLE_SIZE, 0, &this->map));

	// NOTE: destructor for copied buffer is not needed as memory mapping
	// is implicitly unmapped when memory of buffer is destroyed.
}

void CopiedBuffer::cpuCopy(u64 offset, u64 size) {
	ZoneScoped;

	if(!buffer.mem) {
		return;
	}

	if(size == VK_WHOLE_SIZE) {
		dlg_assert(offset <= buffer.size);
		size = buffer.size - offset;
	}

	dlg_assertlm(dlg_level_warn, size < 1024 * 1024, "Large data copies (> 1MB) "
		"will significantly impact performance");
	dlg_assert(offset + size <= buffer.size);

	// TODO: only invalidate when on non-coherent memory
	VkMappedMemoryRange range[1] {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = buffer.mem;
	range[0].size = VK_WHOLE_SIZE;
	VK_CHECK(buffer.dev->dispatch.InvalidateMappedMemoryRanges(buffer.dev->handle, 1, range));

	copy.resize(size);
	std::memcpy(copy.data(), static_cast<const std::byte*>(map) + offset, size);

	copyOffset = offset;
}

// CommandHook
VkCommandBuffer CommandHook::hook(CommandBuffer& hooked,
		Submission& subm, std::unique_ptr<CommandHookSubmission>& data) {
	dlg_assert(hooked.state() == CommandBuffer::State::executable);
	ZoneScoped;

	auto* record = hooked.lastRecordLocked();
	// dlg_assert(record && record->group);

	// Check whether we should attempt to hook this particular record
	bool validTarget =
		record == target.record ||
		&hooked == target.cb ||
		// record->group == target.group;
		target.all;

	if(!validTarget || hierachy_.empty() || !record->commands) {
		return hooked.handle();
	}

	// PERF: only hook when there is something to do.
	// Hook might have no actively needed queries. (Not sure this is
	// really ever the case, we always query time i guess)
	// PERF: in gui, make sure remove hooks when currently not inside
	// cb viewer?

	// Check if it already has a valid record associated
	// TODO(important): before calling find, we'd need to unset the
	// invalidated handles from the commands in hierachy_, might lead
	// to problems at the moment. Make sure we get the CommandRecord in desc().
	// TODO, PERF: when record->hook is true, we only query this
	// to get the new 'match' value (and for debug checks below).
	// Kinda wasted to do all that work.
	auto findRes = find(record->commands, hierachy_, dsState_);
	if(findRes.hierachy.empty()) {
		// Can't find command
		// dlg_warn("Can't hook cb, can't find hooked command");
		return hooked.handle();
	}

	dlg_assert(findRes.hierachy.size() == hierachy_.size());

	// TODO: not possible to reuse the hook-recorded cb when the command
	// buffer uses any update_after_bind descriptors that changed. Track
	// that somehow. See notes in ds.cpp on 'invalidate'.
	if(record->hook) {
		// The record was already submitted - i.e. has a valid state - and
		// the hook is frozen. Nothing to do.
		// PERF: only really just check this here? Likely inefficient,
		// we hook records that are never read by cbGui when it's frozen
		if(freeze) {
			return hooked.handle();
		}

		auto* hookRecord = record->hook.get();
		if(hookRecord->hook == this && hookRecord->hookCounter == counter_) {
			// In this case there is already a pending submission for this
			// record (can happen for simulataneous command buffers).
			// This is a problem since we can't write the pool (and then, when
			// the submission finishes: read buffers) from multiple
			// places. We simply return the original cb in that case,
			// there is a pending submission querying that information after all.
			// NOTE: alternatively, we could create and store a new Record
			// NOTE: alternatively, we could add a semaphore chaining
			//   this submission to the previous one.
			if(hookRecord->state->writer) {
				return hooked.handle();
			}

			dlg_assert(std::equal(
				hookRecord->hcommand.begin(), hookRecord->hcommand.end(),
				findRes.hierachy.begin(), findRes.hierachy.end()));

			// TODO: check whether the 'completed' list already contains
			// this record, see the comment at the list.

			data.reset(new CommandHookSubmission(*hookRecord, subm, findRes.match));
			return hookRecord->cb;
		}
	}

	auto hook = new CommandHookRecord(*this, *record, std::move(findRes.hierachy));
	record->hook.reset(hook);

	data.reset(new CommandHookSubmission(*hook, subm, findRes.match));

	return hook->cb;
}

void CommandHook::desc(std::vector<const Command*> hierachy,
		CommandDescriptorSnapshot dsState, bool invalidate) {
	hierachy_ = std::move(hierachy);
	dsState_ = std::move(dsState);

	if(invalidate) {
		invalidateRecordings();
		invalidateData();
	}
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

void CommandHook::unsetHookOps(bool doQueryTime) {
	this->copyIndexBuffers = false;
	this->copyVertexBuffers = false;
	this->copyXfb = false;
	this->queryTime = doQueryTime;
	this->copyIndirectCmd = false;
	this->copyAttachment = {};
	this->copyDS = {};
	this->copyTransferSrc = false;
	this->copyTransferDst = false;
	invalidateRecordings();
	invalidateData();
}

CommandHook::~CommandHook() {
	invalidateRecordings();
}

// record
CommandHookRecord::CommandHookRecord(CommandHook& xhook,
	CommandRecord& xrecord, std::vector<const Command*> hooked) :
		hook(&xhook), record(&xrecord), hcommand(std::move(hooked)) {

	dlg_assert(!hcommand.empty());
	// this->dev = &xrecord.device();

	this->next = hook->records_;
	if(hook->records_) {
		hook->records_->prev = this;
	}
	hook->records_ = this;

	hookCounter = hook->counter_;

	auto& dev = *xrecord.dev;

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
		auto validBits = dev.queueFamilies[xrecord.queueFamily].props.timestampValidBits;
		if(validBits == 0u) {
			dlg_warn("Queue family {} does not support timing queries", xrecord.queueFamily);
		} else {
			VkQueryPoolCreateInfo qci {};
			qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			qci.queryCount = 2u;
			qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
			VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &this->queryPool));
			nameHandle(dev, this->queryPool, "CommandHookRecord:queryPool");
		}
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

	unsigned maxHookLevel {};
	info.maxHookLevel = &maxHookLevel;

	ZoneScopedN("HookRecord");
	this->hookRecord(record->commands, info);

	dlg_assert(maxHookLevel == hcommand.size() - 1);
	VK_CHECK(dev.dispatch.EndCommandBuffer(this->cb));
}

CommandHookRecord::~CommandHookRecord() {
	ZoneScoped;

	// We can be sure that record is still alive here since when the
	// record is destroyed, all its submissions must have finished as well.
	// And then we would have been destroyed via the finish() command (see
	// the assertions there)
	dlg_assert(record);
	dlg_assert(!state || !state->writer);

	auto& dev = *record->dev;

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
	auto& dev = *record->dev;
	state.reset(new CommandHookState());
	dlg_assert(!hcommand.empty());

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
		 hook->copyIndirectCmd ||
		 (hook->copyTransferDst && dynamic_cast<const ClearAttachmentCmd*>(hcommand.back())));

	if(info.splitRenderPass) {
		auto& rp = *info.beginRenderPassCmd->rp;

		// TODO: we could likely just directly support this
		if(hasChain(*rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
			state->errorMessage = "Splitting multiview renderpass not implemented";
			dlg_trace(state->errorMessage);
			info.splitRenderPass = false;
		}
	}

	if(info.splitRenderPass) {
		auto& desc = *info.beginRenderPassCmd->rp->desc;

		info.beginRenderPassCmd = info.beginRenderPassCmd;
		info.hookedSubpass = info.beginRenderPassCmd->subpassOfDescendant(*hcommand.back());
		dlg_assert(info.hookedSubpass != u32(-1));
		dlg_assert(info.hookedSubpass < desc.subpasses.size());

		// TODO: possible solution for allowing command viewing in this case:
		// - just split up the subpasses into individual renderpasses,
		//   recreate affected pipelines inside the layer and use them
		//   when hooking
		// super ugly and lots of work to implement, could be really
		// expensive and just stall for multiple seconds at worst in large
		// games. Would need extensive testing.
		// This case should only happen anyways when a resolve attachments
		// is used later on (in specific ways, i.e. written and then read
		// or the resolve source written to). Niche feature, looking forward
		// to the reported issue in 5 years.
		if(!splittable(desc, info.hookedSubpass)) {
			info.splitRenderPass = false;
			state->errorMessage = "Can't split render pass (due to resolve attachments)";
			dlg_trace(state->errorMessage);
		} else {
			auto [rpi0, rpi1, rpi2] = splitInterruptable(desc);
			rp0 = create(dev, rpi0);
			rp1 = create(dev, rpi1);
			rp2 = create(dev, rpi2);
		}
	}
}

void CommandHookRecord::hookRecordBeforeDst(Command& dst, const RecordInfo& info) {
	auto& dev = *record->dev;
	dlg_assert(&dst == hcommand.back());

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

		beforeDstOutsideRp(dst, info);

		dlg_assert(rp1);
		auto rpBeginInfo = info.beginRenderPassCmd->info;
		rpBeginInfo.renderPass = rp1;
		// we don't clear anything when starting this rp
		rpBeginInfo.pClearValues = nullptr;
		rpBeginInfo.clearValueCount = 0u;

		if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
			auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
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
		beforeDstOutsideRp(dst, info);
	}
}

void CommandHookRecord::hookRecordAfterDst(Command& dst, const RecordInfo& info) {
	auto& dev = *record->dev;
	dlg_assert(&dst == hcommand.back());

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

		afterDstOutsideRp(dst, info);

		dlg_assert(rp2);
		auto rpBeginInfo = info.beginRenderPassCmd->info;
		rpBeginInfo.renderPass = rp2;
		// we don't clear anything when starting this rp
		rpBeginInfo.pClearValues = nullptr;
		rpBeginInfo.clearValueCount = 0u;

		if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
			auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
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
		afterDstOutsideRp(dst, info);
	}
}

void CommandHookRecord::hookRecordDst(Command& cmd, const RecordInfo& info) {
	auto& dev = *record->dev;

	hookRecordBeforeDst(cmd, info);

	// transform feedback
	auto endXfb = false;
	if(auto drawCmd = dynamic_cast<DrawCmdBase*>(&cmd); drawCmd) {
		if(drawCmd->state.pipe->xfbPatched && hook->copyXfb) {
			dlg_assert(dev.transformFeedback);
			dlg_assert(dev.dispatch.CmdBeginTransformFeedbackEXT);
			dlg_assert(dev.dispatch.CmdBindTransformFeedbackBuffersEXT);
			dlg_assert(dev.dispatch.CmdEndTransformFeedbackEXT);

			// init xfb buffer
			auto xfbSize = 32 * 1024 * 1024; // TODO
			auto usage =
				VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			state->transformFeedback.init(dev, xfbSize, usage);

			auto offset = VkDeviceSize(0u);
			dev.dispatch.CmdBindTransformFeedbackBuffersEXT(cb, 0u, 1u,
				&state->transformFeedback.buffer.buf, &offset,
				&state->transformFeedback.buffer.size);
			dev.dispatch.CmdBeginTransformFeedbackEXT(cb, 0u, 0u, nullptr, nullptr);

			endXfb = true;
		}
	}

	cmd.record(dev, this->cb);

	if(endXfb) {
		dev.dispatch.CmdEndTransformFeedbackEXT(cb, 0u, 0u, nullptr, nullptr);
	}

	auto parentCmd = dynamic_cast<const ParentCommand*>(&cmd);
	auto nextInfo = info;
	if(parentCmd) {
		++nextInfo.nextHookLevel;

		if(queryPool) {
			// timing 0
			auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
		}

		hookRecord(parentCmd->children(), nextInfo);
	}

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
	hookRecordAfterDst(cmd, info);
}

void CommandHookRecord::hookRecord(Command* cmd, const RecordInfo& info) {
	*info.maxHookLevel = std::max(*info.maxHookLevel, info.nextHookLevel);

	auto& dev = *record->dev;
	while(cmd) {
		auto nextInfo = info;

		// check if command is on hooking chain
		if(info.nextHookLevel < hcommand.size() && cmd == hcommand[info.nextHookLevel]) {
			auto hookDst = (info.nextHookLevel == hcommand.size() - 1);
			auto skipRecord = false;

			auto* beginRpCmd = dynamic_cast<BeginRenderPassCmd*>(cmd);
			if(info.splitRenderPass && beginRpCmd) {
				dlg_assert(rp0);
				dlg_assert(!hookDst);
				auto rpBeginInfo = beginRpCmd->info;
				rpBeginInfo.renderPass = rp0;

				if(beginRpCmd->subpassBeginInfo.pNext) {
					auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
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

			if(hookDst) {
				dlg_assert(!skipRecord);
				hookRecordDst(*cmd, info);
			} else {
				auto parentCmd = dynamic_cast<const ParentCommand*>(cmd);
				dlg_assert(hookDst || (parentCmd && parentCmd->children()));

				if(!skipRecord) {
					cmd->record(dev, this->cb);
				}

				if(parentCmd) {
					++nextInfo.nextHookLevel;
					hookRecord(parentCmd->children(), nextInfo);
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
		VkImageLayout srcLayout, const VkImageSubresourceRange& srcSubres,
		std::string& errorMessage, u32 srcQueueFam) {
	if(src.ci.samples != VK_SAMPLE_COUNT_1_BIT) {
		// TODO: support multisampling via vkCmdResolveImage
		//   alternatively we could check if the image is
		//   resolved at the end of the subpass and then simply
		//   copy that.
		errorMessage = "Can't copy/display multisampled attachment";
		dlg_trace(errorMessage);
		return;
	} else if(!src.hasTransferSrc) {
		// There are only very specific cases where this can happen,
		// we could work around some of them (e.g. transient
		// attachment images or swapchain images that don't
		// support transferSrc).
		errorMessage = "Can't display image copy; original can't be copied";
		dlg_trace(errorMessage);
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

	dst.init(dev, src.ci.format, extent, srcSubres.layerCount,
		srcSubres.levelCount, srcSubres.aspectMask, srcQueueFam);
	dst.srcSubresRange = srcSubres;

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

	std::vector<VkImageCopy> copies;
	for(auto m = 0u; m < srcSubres.levelCount; ++m) {
		auto& copy = copies.emplace_back();
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

void performCopy(Device& dev, VkCommandBuffer cb, const Buffer& src,
		VkDeviceSize srcOffset, CopiedBuffer& dst, VkDeviceSize dstOffset,
		VkDeviceSize size) {
	dlg_assert(dstOffset + size <= dst.buffer.size);
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

	dev.dispatch.CmdCopyBuffer(cb, src.handle, dst.buffer.buf, 1, &copy);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
		0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void initAndCopy(Device& dev, VkCommandBuffer cb, CopiedBuffer& dst,
		VkBufferUsageFlags addFlags, Buffer& src,
		VkDeviceSize offset, VkDeviceSize size) {
	dst.init(dev, size, addFlags);
	performCopy(dev, cb, src, offset, dst, 0, size);
}

void CommandHookRecord::copyDs(Command& bcmd, const RecordInfo& info) {
	auto& dev = *record->dev;

	DescriptorState* dsState = nullptr;
	if(auto* drawCmd = dynamic_cast<DrawCmdBase*>(&bcmd)) {
		dsState = &drawCmd->state;
	} else if(auto* dispatchCmd = dynamic_cast<DispatchCmdBase*>(&bcmd)) {
		dsState = &dispatchCmd->state;
	} else {
		state->errorMessage = "Unsupported descriptor command";
		dlg_error("{}", state->errorMessage);
	}

	if(!dsState) {
		return;
	}

	auto [setID, bindingID, elemID, _] = *hook->copyDS;

	// NOTE: we have to check for correct sizes here since the
	// actual command might have changed (for an updated record)
	// and the selected one not valid anymore.
	if(setID >= dsState->descriptorSets.size()) {
		dlg_trace("setID out of range");
		dsState->descriptorSets = {};
		return;
	}

	auto& dsSnapshot = record->lastDescriptorState;

	auto it = dsSnapshot.states.find(dsState->descriptorSets[setID].ds);
	if(it == dsSnapshot.states.end()) {
		dlg_error("Could not find descriptor in snapshot??");
		dsState->descriptorSets = {};
		return;
	}

	auto& ds = nonNull(it->second);
	if(bindingID >= ds.layout->bindings.size()) {
		dlg_trace("bindingID out of range");
		dsState->descriptorSets = {};
		return;
	}

	if(elemID >= descriptorCount(*it->second, bindingID)) {
		dlg_trace("elemID out of range");
		dsState->descriptorSets = {};
		return;
	}

	auto& lbinding = ds.layout->bindings[bindingID];
	auto cat = category(lbinding.descriptorType);
	if(cat == DescriptorCategory::image) {
		auto& elem = images(*it->second, bindingID)[elemID];
		if(needsImageView(lbinding.descriptorType)) {
			auto& imgView = elem.imageView;
			dlg_assert(imgView);
			dlg_assert(imgView->img);
			if(imgView->img) {
				auto& dst = state->dsCopy.emplace<CopiedImage>();

				// We have to handle the special case where a renderpass
				// attachment is bound in a descriptor set (e.g. as
				// input attachment). In that case, it will be
				// in general layout (via our render pass splitting),
				// not in the layout of the ds.
				auto layout = elem.layout;
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
				auto subres = imgView->ci.subresourceRange;
				initAndCopy(dev, cb, dst, *imgView->img, layout, subres,
					state->errorMessage, record->queueFamily);
			}
		} else {
			// we should not land here at all! Check state in
			// cb gui before registring hook. Don't register a hook
			// just to find out *here* that we don't need it
			state->errorMessage = "Just a sampler bound";
			dlg_error(state->errorMessage);
		}
	} else if(cat == DescriptorCategory::buffer) {
		auto& elem = buffers(*it->second, bindingID)[elemID];
		dlg_assert(elem.buffer);

		auto& dst = state->dsCopy.emplace<CopiedBuffer>();
		auto range = elem.range;
		if(range == VK_WHOLE_SIZE) {
			range = elem.buffer->ci.size - elem.offset;
		}

		auto size = std::min(maxBufCopySize, range);
		initAndCopy(dev, cb, dst, 0u, nonNull(elem.buffer),
			elem.offset, size);
	} else if(cat == DescriptorCategory::bufferView) {
		// TODO: copy as buffer or image? maybe best to copy
		//   as buffer but then create bufferView on our own?
		// auto& dst = state->dsCopy.emplace<CopiedBuffer>();
		// dlg_assert(elem.bufferView->buffer);
		// copyBuffer(dst, elem.bufferView->buffer->handle,
		// 	elem.bufferView->ci.offset, elem.bufferView->ci.range);
		state->errorMessage = "BufferView ds copy unimplemented";
		dlg_error(state->errorMessage);
	}
}

void CommandHookRecord::copyAttachment(const RecordInfo& info, unsigned attID) {
	auto& dev = *record->dev;

	dlg_assert(info.beginRenderPassCmd);
	auto& fb = nonNull(info.beginRenderPassCmd->fb);

	if(attID >= fb.attachments.size()) {
		state->errorMessage = "attachment out of range";
		dlg_trace("copyAttachment {} out of range ({})", attID, fb.attachments.size());
		hook->copyAttachment = {};
	} else {
		auto& imageView = fb.attachments[attID];
		dlg_assert(imageView);
		dlg_assert(imageView->img);
		auto* image = imageView->img;

		if(!image) {
			// NOTE: this should not happen at all, not a regular error.
			dlg_error("ImageView has no associated image");
		} else {
			auto& srcImg = *image;
			auto layout = VK_IMAGE_LAYOUT_GENERAL; // layout between rp splits, see rp.cpp

			// TODO: select exact layer/mip in view range via gui
			auto& subres = imageView->ci.subresourceRange;
			initAndCopy(dev, cb, state->attachmentCopy, srcImg, layout, subres,
				state->errorMessage, record->queueFamily);
		}
	}
}

VkImageSubresourceRange fullSubresRange(const Image& img) {
	VkImageSubresourceRange ret {};
	if(FormatIsColor(img.ci.format)) {
		ret.aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
	}
	if(FormatHasDepth(img.ci.format)) {
		ret.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	if(FormatHasStencil(img.ci.format)) {
		ret.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	ret.levelCount = img.ci.mipLevels;
	ret.layerCount = img.ci.arrayLayers;
	return ret;
}

VkImageSubresourceRange toRange(const VkImageSubresourceLayers& subres) {
	VkImageSubresourceRange ret {};
	ret.aspectMask = subres.aspectMask;
	ret.baseArrayLayer = subres.baseArrayLayer;
	ret.layerCount = subres.layerCount;
	ret.baseMipLevel = subres.mipLevel;
	ret.levelCount = 1u;

	return ret;
}

void CommandHookRecord::copyTransfer(Command& bcmd, const RecordInfo& info) {
	dlg_assert(hook->copyTransferDst != hook->copyTransferSrc);
	auto& dev = *record->dev;

	if(hook->copyTransferSrc) {
		VkImageSubresourceRange subres {};
		VkImageLayout layout {};
		Image* src {};

		if(auto* cmd = dynamic_cast<const CopyImageCmd*>(&bcmd); cmd) {
			src = cmd->src;
			layout = cmd->srcLayout;
			subres = toRange(cmd->copies[0].srcSubresource);
		} else if(auto* cmd = dynamic_cast<const BlitImageCmd*>(&bcmd); cmd) {
			src = cmd->src;
			layout = cmd->srcLayout;
			subres = toRange(cmd->blits[0].srcSubresource);
		} else if(auto* cmd = dynamic_cast<const CopyImageToBufferCmd*>(&bcmd); cmd) {
			src = cmd->src;
			layout = cmd->srcLayout;
			subres = toRange(cmd->copies[0].imageSubresource);
		} else if(auto* cmd = dynamic_cast<const ResolveImageCmd*>(&bcmd); cmd) {
			src = cmd->src;
			layout = cmd->srcLayout;
			subres = toRange(cmd->regions[0].srcSubresource);
		}

		dlg_assert(src);
		initAndCopy(dev, cb, state->transferImgCopy, *src,
			layout, subres, state->errorMessage, record->queueFamily);
	} else if(hook->copyTransferDst) {
		VkImageSubresourceRange subres {};
		VkImageLayout layout {};
		Image* src {};

		if(auto* cmd = dynamic_cast<const CopyImageCmd*>(&bcmd); cmd) {
			src = cmd->dst;
			layout = cmd->dstLayout;
			subres = toRange(cmd->copies[0].dstSubresource);
		} else if(auto* cmd = dynamic_cast<const BlitImageCmd*>(&bcmd); cmd) {
			src = cmd->dst;
			layout = cmd->dstLayout;
			subres = toRange(cmd->blits[0].dstSubresource);
		} else if(auto* cmd = dynamic_cast<const CopyBufferToImageCmd*>(&bcmd); cmd) {
			src = cmd->dst;
			layout = cmd->dstLayout;
			subres = toRange(cmd->copies[0].imageSubresource);
		} else if(auto* cmd = dynamic_cast<const ResolveImageCmd*>(&bcmd); cmd) {
			src = cmd->dst;
			layout = cmd->dstLayout;
			subres = toRange(cmd->regions[0].dstSubresource);
		} else if(auto* cmd = dynamic_cast<const ClearColorImageCmd*>(&bcmd); cmd) {
			src = cmd->dst;
			layout = cmd->dstLayout;
			subres = cmd->ranges[0];
		} else if(auto* cmd = dynamic_cast<const ClearDepthStencilImageCmd*>(&bcmd); cmd) {
			src = cmd->dst;
			layout = cmd->dstLayout;
			subres = cmd->ranges[0];
		} else if(auto* cmd = dynamic_cast<const ClearAttachmentCmd*>(&bcmd)) {
			auto& rp = nonNull(info.beginRenderPassCmd->rp);
			auto& fb = nonNull(info.beginRenderPassCmd->fb);

			// TODO: support showing multiple cleared attachments in gui,
			//   allowing to select here which one is copied.
			auto& clearAtt = cmd->attachments[0];
			u32 attID = clearAtt.colorAttachment;
			if(clearAtt.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
				auto& depthStencil = nonNull(rp.desc->subpasses[info.hookedSubpass].pDepthStencilAttachment);
				attID = depthStencil.attachment;
			}

			dlg_assert(fb.attachments.size() > attID);
			auto& imgView = nonNull(fb.attachments[attID]);
			auto& img = nonNull(imgView.img);

			// image must be in general layout because we are just between
			// the split render passes
			src = &img;
			layout = VK_IMAGE_LAYOUT_GENERAL;
			subres = imgView.ci.subresourceRange;
		}

		dlg_assert(src);
		initAndCopy(dev, cb, state->transferImgCopy, *src,
			layout, subres, state->errorMessage, record->queueFamily);
	}
}

void CommandHookRecord::beforeDstOutsideRp(Command& bcmd, const RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "beforeDstOutsideRp");

	if(info.splitRenderPass) {
		// TODO: kinda hacky, can be improved. But we definitely need a general barrier here,
		// between the render passes to make sure the first render pass really
		// has finished (with *everything*, not just the stuff we are interested
		// in here) before we start the second one.
		VkMemoryBarrier memBarrier {};
		memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		memBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 1, &memBarrier, 0, nullptr, 0, nullptr);
	}

	// indirect copy
	if(hook->copyIndirectCmd) {
		if(auto* cmd = dynamic_cast<DrawIndirectCmd*>(&bcmd)) {
			VkDeviceSize stride = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd->stride ? cmd->stride : stride;
			auto dstSize = cmd->drawCount * stride;
			initAndCopy(dev, cb, state->indirectCopy,  0u,
				nonNull(cmd->buffer), cmd->offset, dstSize);
		} else if(auto* cmd = dynamic_cast<DispatchIndirectCmd*>(&bcmd)) {
			auto size = sizeof(VkDispatchIndirectCommand);
			initAndCopy(dev, cb, state->indirectCopy, 0u,
				nonNull(cmd->buffer), cmd->offset, size);
		} else if(auto* cmd = dynamic_cast<DrawIndirectCountCmd*>(&bcmd)) {
			auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto size = 4 + cmd->maxDrawCount * cmdSize;
			state->indirectCopy.init(dev, size, 0u);

			// copy count
			performCopy(dev, cb, nonNull(cmd->countBuffer), cmd->countBufferOffset,
				state->indirectCopy, 0u, 4u);
			// copy commands
			// NOTE: using an indirect-transfer-emulation approach (see
			// below, same problem as for indirect draw vertex/index bufs)
			// we could avoid copying too much data here. Likely not worth
			// it here though unless application pass *huge* maxDrawCount
			// values (which they shouldn't).
			performCopy(dev, cb, nonNull(cmd->buffer), cmd->offset,
				state->indirectCopy, 4u, cmd->maxDrawCount * cmdSize);
		} else {
			state->errorMessage = "Unsupported indirect command";
			dlg_warn(state->errorMessage);
		}
	}

	// attachment
	if(hook->copyAttachment && hook->copyAttachment->before) {
		copyAttachment(info, hook->copyAttachment->id);
	}

	// descriptor state
	if(hook->copyDS && hook->copyDS->before) {
		copyDs(bcmd, info);
	}

	auto* drawCmd = dynamic_cast<DrawCmdBase*>(&bcmd);

	// PERF: we could support tighter buffer bounds for indirect/indexed draw
	// calls. See node 1749 for a sketch using a couple of compute shaders,
	// basically emulating an indirect transfer.
	// PERF: for non-indexed/non-indirect draw calls we know the exact
	// sizes of vertex/index buffers to copy, we could use that.
	auto maxVertIndSize = maxBufCopySize;

	if(hook->copyVertexBuffers) {
		dlg_assert(drawCmd);
		for(auto& vertbuf : drawCmd->state.vertices) {
			auto& dst = state->vertexBufCopies.emplace_back();
			if(!vertbuf.buffer) {
				continue;
			}

			auto size = std::min(maxVertIndSize, vertbuf.buffer->ci.size - vertbuf.offset);
			initAndCopy(dev, cb, dst, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				nonNull(vertbuf.buffer), vertbuf.offset, size);
		}
	}

	if(hook->copyIndexBuffers) {
		dlg_assert(drawCmd);
		auto& inds = drawCmd->state.indices;
		if(inds.buffer) {
			auto size = std::min(maxVertIndSize, inds.buffer->ci.size - inds.offset);
			initAndCopy(dev, cb, state->indexBufCopy, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				nonNull(inds.buffer), inds.offset, size);
		}
	}

	// transfer
	if(hook->copyTransferBefore && (hook->copyTransferDst || hook->copyTransferSrc)) {
		copyTransfer(bcmd, info);
	}
}

void CommandHookRecord::afterDstOutsideRp(Command& bcmd, const RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "afterDsOutsideRp");

	if(info.splitRenderPass) {
		// TODO: kinda hacky, can be improved. But we definitely need a general barrier here,
		// between the render passes to make sure the second render pass really
		// has finished (with *everything*, not just the stuff we are interested
		// in here) before we start the third one.
		VkMemoryBarrier memBarrier {};
		memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		memBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 1, &memBarrier, 0, nullptr, 0, nullptr);
	}

	// attachment
	if(hook->copyAttachment && !hook->copyAttachment->before) {
		copyAttachment(info, hook->copyAttachment->id);
	}

	// descriptor state
	if(hook->copyDS && !hook->copyDS->before) {
		copyDs(bcmd, info);
	}

	// transfer
	if(!hook->copyTransferBefore && (hook->copyTransferDst || hook->copyTransferSrc)) {
		copyTransfer(bcmd, info);
	}
}

void CommandHookRecord::finish() noexcept {
	// NOTE: We don't do this since we can assume the record to remain
	// valid until all submissions are finished. We can assume it to
	// be valid throughout the entire lifetime of *this.
	// record = nullptr;

	// Keep alive when there still a pending submission.
	// It will delete this record then instead.
	if(!nonNull(state).writer) {
		delete this;
	} else {
		// no other reason for this record to be finished except
		// invalidation
		dlg_assert(!hook);
	}
}

// submission
CommandHookSubmission::CommandHookSubmission(CommandHookRecord& rec,
		Submission& subm, float xmatch) : record(&rec), match(xmatch) {
	dlg_assert(rec.state);
	dlg_assert(!rec.state->writer);
	rec.state->writer = &subm;
	descriptorSnapshot = rec.record->lastDescriptorState;
}

CommandHookSubmission::~CommandHookSubmission() {
}

void CommandHookSubmission::finish(Submission& subm) {
	ZoneScoped;

	dlg_assert(record && record->record);

	dlg_assert(record->state);
	dlg_assert(record->state->writer == &subm);
	record->state->writer = nullptr;

	// In this case the hook was removed, no longer interested in results.
	// Since we are the only submission left to the record, it can be
	// destroyed.
	if(!record->hook) {
		delete record;
		return;
	}

	transmitTiming();

	auto& state = record->hook->completed.emplace_back();
	state.record = IntrusivePtr<CommandRecord>(record->record);
	state.match = this->match;
	state.state = record->state;
	state.command = record->hcommand;
	state.descriptorSnapshot = std::move(this->descriptorSnapshot);

	dlg_assertm(record->hook->completed.size() < 32,
		"Hook state overflow detected");

	auto maxCopySize = 64 * 1024;

	auto cpuCopyMin = [&](auto& buf) {
		auto size = std::min<VkDeviceSize>(buf.buffer.size, maxCopySize);
		buf.cpuCopy(0, size);
	};

	// Copy potential buffers. We don't check for hookOps here as
	// buffers aren't created in the first place (making cpuCopy a noop)
	// when the readback op isn't active.
	// TODO: only copy the requested region, what we are currently
	// viewing and therefore *absolutely* need on cpu.
	if(auto* buf = std::get_if<CopiedBuffer>(&record->state->dsCopy)) {
		cpuCopyMin(*buf);
	}

	cpuCopyMin(record->state->indexBufCopy);
	cpuCopyMin(record->state->transformFeedback);

	for(auto& vbuf : record->state->vertexBufCopies) {
		cpuCopyMin(vbuf);
	}

	// indirect command readback
	if(record->hook->copyIndirectCmd) {
		auto& bcmd = *record->hcommand.back();

		if(auto* cmd = dynamic_cast<const DrawIndirectCountCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.buffer.size >= 4u);
			auto* count = static_cast<const u32*>(record->state->indirectCopy.map);
			record->state->indirectCommandCount = *count;
			dlg_assert(record->state->indirectCommandCount <= cmd->maxDrawCount);

			auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assertlm(dlg_level_warn,
				record->state->indirectCopy.buffer.size >= 4 + *count * cmdSize,
				"Indirect command readback buffer too small; commands missing");

			auto cmdsSize = cmdSize * record->state->indirectCommandCount;
			record->state->indirectCopy.cpuCopy(4u, cmdsSize);
			record->state->indirectCopy.copyOffset = 0u;
		} else if(auto* cmd = dynamic_cast<const DrawIndirectCmd*>(&bcmd)) {
			[[maybe_unused]] auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assert(record->state->indirectCopy.buffer.size ==
				cmd->drawCount * cmdSize);

			record->state->indirectCommandCount = cmd->drawCount;
			record->state->indirectCopy.cpuCopy();
		} else if(dynamic_cast<const DispatchIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.buffer.size ==
				sizeof(VkDispatchIndirectCommand));

			record->state->indirectCommandCount = 1u;
			record->state->indirectCopy.cpuCopy();
		} else {
			dlg_warn("Unsupported indirect command (readback)");
		}
	}
}

void CommandHookSubmission::transmitTiming() {
	ZoneScoped;

	auto& dev = *record->record->dev;

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
	record->state->neededTime = diff;
}

} // namespace vil
