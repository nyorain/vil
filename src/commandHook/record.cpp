#include <commandHook/record.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/copy.hpp>
#include <command/record.hpp>
#include <command/commands.hpp>
#include <util/util.hpp>
#include <util/fmt.hpp>
#include <device.hpp>
#include <stats.hpp>
#include <buffer.hpp>
#include <queue.hpp>
#include <image.hpp>
#include <swapchain.hpp>
#include <pipe.hpp>
#include <accelStruct.hpp>
#include <cb.hpp>
#include <rp.hpp>
#include <ds.hpp>
#include <vk/format_utils.h>

namespace vil {

// record
CommandHookRecord::CommandHookRecord(CommandHook& xhook,
	CommandRecord& xrecord, std::vector<const Command*> hooked,
	const CommandDescriptorSnapshot& descriptors,
	const CommandHookOps& ops, LocalCapture* xlocalCapture) :
		hook(&xhook), record(&xrecord), hcommand(std::move(hooked)) {

	++DebugStats::get().aliveHookRecords;
	assertOwned(xhook.dev_->mutex);

	this->localCapture = xlocalCapture;
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

	VK_CHECK_DEV(dev.dispatch.AllocateCommandBuffers(dev.handle, &allocInfo, &this->cb), dev);
	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, this->cb);
	nameHandle(dev, this->cb, "CommandHookRecord:cb");

	// query pool
	if(ops.queryTime) {
		auto validBits = dev.queueFamilies[xrecord.queueFamily].props.timestampValidBits;
		if(validBits == 0u) {
			dlg_info("Queue family {} does not support timing queries", xrecord.queueFamily);
		} else {
			VkQueryPoolCreateInfo qci {};
			qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			qci.queryCount = 2u;
			qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
			VK_CHECK_DEV(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &this->queryPool), dev);
			nameHandle(dev, this->queryPool, "CommandHookRecord:queryPool");
		}
	}

	RecordInfo info {ops};
	info.descriptors = &descriptors;
	initState(info);

	// TODO
	// this->dsState.resize(ops.descriptorCopies.size());

	// record
	// we can never submit the cb simulataneously anyways, see CommandHook
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK_DEV(dev.dispatch.BeginCommandBuffer(this->cb, &cbbi), dev);

	// initial cmd stuff
	if(this->queryPool) {
		dev.dispatch.CmdResetQueryPool(cb, queryPool, 0, 2);
	}

	unsigned maxHookLevel {};
	info.maxHookLevel = &maxHookLevel;

	ZoneScopedN("HookRecord");
	this->hookRecord(record->commands, info);

	VK_CHECK_DEV(dev.dispatch.EndCommandBuffer(this->cb), dev);

	if(!hcommand.empty()) {
		dlg_assert(maxHookLevel >= hcommand.size() - 1);
		dlg_assert(dynamic_cast<const ParentCommand*>(hcommand.back()) ||
			maxHookLevel == hcommand.size() - 1);
	}
}

CommandHookRecord::~CommandHookRecord() {
	ZoneScoped;

	dlg_assert(DebugStats::get().aliveHookRecords > 0);
	--DebugStats::get().aliveHookRecords;

	// We can be sure that record is still alive here since when the
	// record is destroyed, all its submissions must have finished as well.
	// And then we would have been destroyed via the finish() command (see
	// the assertions there)
	dlg_assert(record);
	dlg_assert(!writer);

	auto& dev = *record->dev;

	// TODO: don't require this here. Instead, require that it's not
	// locked, enfore that everywhere and make sure to lock it here
	// only where it's needed.
	assertOwned(dev.mutex);

	// destroy resources
	auto commandPool = dev.queueFamilies[record->queueFamily].commandPool;

	for(auto imgView : imageViews) {
		dev.dispatch.DestroyImageView(dev.handle, imgView, nullptr);
	}

	for(auto bufView : bufferViews) {
		dev.dispatch.DestroyBufferView(dev.handle, bufView, nullptr);
	}

	if(!descriptorSets.empty()) {
		dev.dispatch.FreeDescriptorSets(dev.handle, dev.dsPool,
			u32(descriptorSets.size()), descriptorSets.data());
	}

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
	if(hcommand.empty()) {
		return;
	}

	auto& dev = *record->dev;

	state.reset(new CommandHookState());
	state->copiedAttachments.resize(info.ops.attachmentCopies.size());
	state->copiedDescriptors.resize(info.ops.descriptorCopies.size());

	// Find out if final hooked command is inside render pass
	const auto hookClearAttachment =
		 (info.ops.copyTransferDstBefore || info.ops.copyTransferDstAfter)
		  	&& commandCast<const ClearAttachmentCmd*>(hcommand.back());
	const auto careAboutRendering =
		info.ops.copyVertexBuffers ||
		 info.ops.copyIndexBuffers ||
		 !info.ops.attachmentCopies.empty() ||
		 !info.ops.descriptorCopies.empty() ||
		 info.ops.copyIndirectCmd ||
		 hookClearAttachment;

	auto preEnd = hcommand.end() - 1;
	info.hookedSubpass = u32(-1);

	for(auto it = hcommand.begin(); it != preEnd; ++it) {
		auto* cmd = *it;

		auto category = cmd->category();
		if(category == CommandCategory::beginRenderPass) {
			info.beginRenderPassCmd = deriveCast<const BeginRenderPassCmd*>(cmd);
		} else if(category == CommandCategory::renderSection) {
			info.rpi = &deriveCast<const RenderSectionCommand*>(cmd)->rpi;

			if(info.beginRenderPassCmd) {
				info.hookedSubpass = deriveCast<const SubpassCmd*>(cmd)->subpassID;
			} else {
				info.beginRenderingCmd = deriveCast<const BeginRenderingCmd*>(cmd);
			}

			break;
		}
	}

	// some operations (index/vertex/attachment) copies only make sense
	// inside a render pass.
	dlg_assert(info.rpi ||
		(!info.ops.copyVertexBuffers &&
		 !info.ops.copyIndexBuffers &&
		 !hookClearAttachment &&
		 info.ops.attachmentCopies.empty()));

	// when the hooked command is inside a render pass and we need to perform
	// operations (e.g. copies) not possible while inside a render pass,
	// we have to split the render pass around the selected command.
	if(careAboutRendering && info.beginRenderPassCmd) {
		auto& rp = *info.beginRenderPassCmd->rp;
		auto& desc = rp.desc;

		dlg_assert(info.hookedSubpass != u32(-1));
		dlg_assert(info.hookedSubpass < desc.subpasses.size());

		// TODO: we could likely just directly support this (with exception
		// of transform feedback maybe)
		if(hasChain(rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
			dlg_warn("Splitting multiview renderpass not implemented");
		} else {
			// TODO: possible solution for allowing command viewing when
			// rp is not spittable:
			// - just split up the subpasses into individual renderpasses,
			//   recreate affected pipelines inside the layer and use them
			//   when hooking
			// super ugly and lots of work to implement, could be really
			// expensive and just stall for multiple seconds at worst in large
			// games. Would need extensive testing.
			// This case should only happen anyways when a resolve attachments
			// is used later on (in specific ways, i.e. written and then read
			// or the resolve source written to). Niche feature, am already
			// looking forward to the reported issue in 5 years.
			if(!splittable(desc, info.hookedSubpass)) {
				dlg_warn("Can't split render pass (due to resolve attachments)");
			} else {
				info.splitRendering = true;
				auto [rpi0, rpi1, rpi2] = splitInterruptable(desc);
				rp0 = create(dev, rpi0);
				rp1 = create(dev, rpi1);
				rp2 = create(dev, rpi2);
			}
		}
	} else if(careAboutRendering && info.beginRenderingCmd) {
		info.splitRendering = true;
	}
}

void CommandHookRecord::dispatchRecord(Command& cmd, RecordInfo& info) {
	auto& dev = *record->dev;

	if(info.rebindComputeState && cmd.category() == CommandCategory::dispatch) {
		auto& dcmd = static_cast<const DispatchCmdBase&>(cmd);

		// pipe, descriptors
		bind(dev, this->cb, *dcmd.state);

		// push constants
		if(!dcmd.pushConstants.data.empty()) {
			auto data = dcmd.pushConstants.data;
			auto& layout = dcmd.state->pipe->layout;
			for(auto& pcr : layout->pushConstants) {
				if(!(pcr.stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) ||
						pcr.offset >= data.size()) {
					continue;
				}

				auto size = std::min<u32>(pcr.size, data.size() - pcr.offset);
				dev.dispatch.CmdPushConstants(cb, layout->handle,
					pcr.stageFlags, pcr.offset, size, data.data() + pcr.offset);
			}
		}

		info.rebindComputeState = false;
	}

	cmd.record(*record->dev, this->cb, this->record->queueFamily);
}

void CommandHookRecord::hookRecordBeforeDst(Command& dst, RecordInfo& info) {
	auto& dev = *record->dev;

	dlg_assert(&dst == hcommand.back());

	if(info.splitRendering && info.beginRenderPassCmd) {
		dlg_assert(info.rpi);

		auto numSubpasses = info.beginRenderPassCmd->rp->desc.subpasses.size();
		for(auto i = info.hookedSubpass; i + 1 < numSubpasses; ++i) {
			// Subpass contents irrelevant here.
			// TODO: missing potential forward of pNext chain here
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

		// we never actually record CmdExecuteCommands when hook-recording,
		// so always pass inline here.
		auto subpassBeginInfo = info.beginRenderPassCmd->subpassBeginInfo;
		subpassBeginInfo.contents = VK_SUBPASS_CONTENTS_INLINE;

		if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
			auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
			dlg_assert(beginRp2);
			beginRp2(cb, &rpBeginInfo, &subpassBeginInfo);
		} else {
			dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo, subpassBeginInfo.contents);
		}

		for(auto i = 0u; i < info.hookedSubpass; ++i) {
			// TODO: missing potential forward of pNext chain here.
			// Subpass contents irrelevant here.
			dev.dispatch.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
		}
	} else if(info.splitRendering && info.beginRenderingCmd) {
		dlg_assert(info.rpi);

		dev.dispatch.CmdEndRendering(cb);

		beforeDstOutsideRp(dst, info);

		info.beginRenderingCmd->record(dev, cb,
			true,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			VK_ATTACHMENT_STORE_OP_STORE);
	} else if(!info.rpi) {
		// NOTE that this includes NextSubpass commands, so
		//   info.BeginRenderPassCmd might still be non-null
		beforeDstOutsideRp(dst, info);
	} else {
		// no-op, we land here when we couldn't split the renderpass :(
		dlg_assert(!info.splitRendering && info.beginRenderPassCmd);
	}
}

void CommandHookRecord::hookRecordAfterDst(Command& dst, RecordInfo& info) {
	auto& dev = *record->dev;
	dlg_assert(&dst == hcommand.back());

	if(info.splitRendering && info.beginRenderPassCmd) {
		dlg_assert(info.rpi);

		// TODO: missing potential forward of pNext chain here
		auto numSubpasses = info.beginRenderPassCmd->rp->desc.subpasses.size();
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

		// we never actually record CmdExecuteCommands when hook-recording,
		// so always pass inline here.
		auto subpassBeginInfo = info.beginRenderPassCmd->subpassBeginInfo;
		subpassBeginInfo.contents = VK_SUBPASS_CONTENTS_INLINE;

		if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
			auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
			dlg_assert(beginRp2);
			beginRp2(cb, &rpBeginInfo, &subpassBeginInfo);
		} else {
			dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo, subpassBeginInfo.contents);
		}

		for(auto i = 0u; i < info.hookedSubpass; ++i) {
			// TODO: missing potential forward of pNext chain here
			// TODO: subpass contents relevant?
			dev.dispatch.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
		}
	} else if(info.splitRendering && info.beginRenderingCmd) {
		dlg_assert(info.rpi);

		dev.dispatch.CmdEndRendering(cb);

		afterDstOutsideRp(dst, info);

		info.beginRenderingCmd->record(dev, cb,
			false,
			VK_ATTACHMENT_LOAD_OP_LOAD,
			std::nullopt);
	} else if(!info.rpi) {
		// we are not inside a render pass instance.
		// NOTE that this includes NextSubpass commands, so
		//   info.BeginRenderPassCmd might still be non-null
		afterDstOutsideRp(dst, info);
	} else {
		// no-op, we land here when we couldn't split the renderpass :(
		dlg_assert(!info.splitRendering && info.beginRenderPassCmd);
	}
}

void CommandHookRecord::hookRecordDst(Command& cmd, RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel cblbl(dev, cb, "vil:hookRecordDst");

	hookRecordBeforeDst(cmd, info);

	// transform feedback
	auto endXfb = false;
	if(cmd.category() == CommandCategory::draw) {
		auto* drawCmd = deriveCast<DrawCmdBase*>(&cmd);
		dlg_assert(drawCmd->state->pipe);

		if(drawCmd->state->pipe->xfbPatch && info.ops.copyXfb) {
			dlg_assert(dev.transformFeedback);
			dlg_assert(dev.dispatch.CmdBeginTransformFeedbackEXT);
			dlg_assert(dev.dispatch.CmdBindTransformFeedbackBuffersEXT);
			dlg_assert(dev.dispatch.CmdEndTransformFeedbackEXT);

			// init xfb buffer
			auto xfbSize = 32 * 1024 * 1024; // TODO
			auto usage =
				VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			state->transformFeedback.ensure(dev, xfbSize, usage);

			auto offset = VkDeviceSize(0u);
			dev.dispatch.CmdBindTransformFeedbackBuffersEXT(cb, 0u, 1u,
				&state->transformFeedback.buf, &offset,
				&state->transformFeedback.size);
			dev.dispatch.CmdBeginTransformFeedbackEXT(cb, 0u, 0u, nullptr, nullptr);

			endXfb = true;
		}
	}

	// TODO: Improve the timing queries for draw commands. With proper
	// subpass dependencies and barrier stages we can probably isolate
	// draw commands better (especially in the case where we don't
	// split the render pass).
	if(queryPool && timingBarrierBefore && !info.beginRenderPassCmd) {
		// Make sure the timing query only captures the command itself,
		// not stuff that comes before it
		VkMemoryBarrier barrier {};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u,
			1u, &barrier, 0u, nullptr, 0u, nullptr);

		// add a dummy command to make sure the pipeline barrier is effective
		// TODO: ugly workaround needed in case cmd is something like
		// a debug label command (at least in that case it was observed to
		// be effective, radv mesa 21). Not sure atm how to properly fix this,
		// maybe we only need this because of a driver bug?
		if(!info.splitRendering) {
			dummyBuf.ensure(dev, 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
			dev.dispatch.CmdFillBuffer(cb, dummyBuf.buf, 0, 4, 42u);
		}
	}

	dispatchRecord(cmd, info);

	auto cmdAsParent = dynamic_cast<const ParentCommand*>(&cmd);
	auto nextInfo = info;

	if(queryPool) { // timing 0
		auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
	}

	if(cmdAsParent) {
		++nextInfo.nextHookLevel;
		hookRecord(cmdAsParent->children(), nextInfo);
	}

	if(queryPool) { // timing 1
		auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dev.dispatch.CmdWriteTimestamp(this->cb, stage1, this->queryPool, 1);

		if(timingBarrierAfter && !info.beginRenderPassCmd) {
			// Make sure the timing query only captures the command itself,
			// not stuff that comes after it
			VkMemoryBarrier barrier {};
			barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u,
				1u, &barrier, 0u, nullptr, 0u, nullptr);
		}
	}

	if(endXfb) {
		dev.dispatch.CmdEndTransformFeedbackEXT(cb, 0u, 0u, nullptr, nullptr);
	}

	// render pass split: rp2
	hookRecordAfterDst(cmd, info);
}

void CommandHookRecord::hookRecord(Command* cmd, RecordInfo& info) {
	*info.maxHookLevel = std::max(*info.maxHookLevel, info.nextHookLevel);

	auto& dev = *record->dev;
	while(cmd) {
		// check if command needs additional, manual hook
		if(cmd->category() == CommandCategory::buildAccelStruct && hook->hookAccelStructBuilds) {

			auto* basCmd = commandCast<BuildAccelStructsCmd*>(cmd);
			auto* basCmdIndirect = commandCast<BuildAccelStructsCmd*>(cmd);
			dlg_assert(basCmd || basCmdIndirect);

			if(basCmd) {
				hookBefore(*basCmd);
			} else if(basCmdIndirect) {
				hookBefore(*basCmdIndirect);
			}

			// We have to restore the original compute state here since the
			// the acceleration structure copies change it.
			info.rebindComputeState = true;
		}

		if(auto* cas = commandCast<CopyAccelStructCmd*>(cmd); cas) {
			accelStructOps.push_back(AccelStructCopy{cas->src, cas->dst});
		}

		// check if command is on hooking chain
		if(info.nextHookLevel < hcommand.size() && cmd == hcommand[info.nextHookLevel]) {
			auto hookDst = (info.nextHookLevel == hcommand.size() - 1);
			auto skipRecord = false;

			// hook BeginRenderPass
			if(info.beginRenderPassCmd == cmd && info.splitRendering) {
				dlg_assert(rp0);
				dlg_assert(!hookDst);
				auto rpBeginInfo = info.beginRenderPassCmd->info;
				rpBeginInfo.renderPass = rp0;

				// we never actually record CmdExecuteCommands when hook-recording,
				// so always pass inline here.
				auto subpassBeginInfo = info.beginRenderPassCmd->subpassBeginInfo;
				subpassBeginInfo.contents = VK_SUBPASS_CONTENTS_INLINE;

				if(info.beginRenderPassCmd->subpassBeginInfo.pNext) {
					auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
					dlg_assert(beginRp2);
					beginRp2(cb, &rpBeginInfo, &subpassBeginInfo);
				} else {
					dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo, subpassBeginInfo.contents);
				}

				skipRecord = true;
			}

			// hook BeginRendering
			if(info.beginRenderingCmd == cmd && info.splitRendering) {
				dlg_assert(!hookDst);

				info.beginRenderingCmd->record(dev, cb, true,
					std::nullopt, VK_ATTACHMENT_STORE_OP_STORE);

				skipRecord = true;
			}

			if(hookDst) {
				dlg_assert(!skipRecord);
				hookRecordDst(*cmd, info);
			} else {
				auto parentCmd = dynamic_cast<const ParentCommand*>(cmd);
				dlg_assert(hookDst || (parentCmd && parentCmd->children()));

				if(!skipRecord) {
					dispatchRecord(*cmd, info);
				}

				if(parentCmd) {
					++info.nextHookLevel;
					hookRecord(parentCmd->children(), info);
					--info.nextHookLevel;
				}
			}
		} else {
			dispatchRecord(*cmd, info);
			if(auto parentCmd = dynamic_cast<const ParentCommand*>(cmd); parentCmd) {
				hookRecord(parentCmd->children(), info);
			}
		}

		cmd = cmd->next;
	}
}

void CommandHookRecord::copyDs(Command& bcmd, RecordInfo& info,
		const DescriptorCopyOp& copyDesc, unsigned dstID,
		CommandHookState::CopiedDescriptor& dst,
		IntrusivePtr<DescriptorSetCow>& dstCow) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:copyDs");

	dst.op = copyDesc;

	dlg_assert_or(bcmd.category() == CommandCategory::draw ||
		bcmd.category() == CommandCategory::dispatch ||
		bcmd.category() == CommandCategory::traceRays,
		return);

	const DescriptorState& dsState =
		static_cast<const StateCmdBase&>(bcmd).boundDescriptors();

	auto [setID, bindingID, elemID, _1, imageAsBuffer] = copyDesc;

	// NOTE: we have to check for correct sizes here since the
	// actual command might have changed (for an updated record)
	// and the selected one not valid anymore.
	if(setID >= dsState.descriptorSets.size()) {
		dlg_error("setID out of range: {}/{}", setID, dsState.descriptorSets.size());
		return;
	}

	auto it = info.descriptors->states.find(dsState.descriptorSets[setID].dsEntry);
	if(it == info.descriptors->states.end()) {
		dlg_error("Could not find descriptor in snapshot??");
		return;
	}

	dstCow = it->second;
	auto [ds, lock] = access(*it->second);

	if(bindingID >= ds.layout->bindings.size()) {
		dlg_trace("bindingID out of range");
		return;
	}

	if(ds.layout->bindings[bindingID].flags & VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) {
		// TODO: we could make this work but it's not easy. The main problem is
		// that we have no guarantees for the handle we are reading here staying valid.
		// At the time of the submission, a binding could e.g. contain a buffer
		// that gets destroyed during submission (valid usage for update_unused_while_pending)
		// so we can't just use it here.
		// We would have to track the update_unused_while_pending handles that are used
		// somehow and when one of them is destroyed, wait for the associated
		// hooked submission. No way around this I guess.
		dlg_trace("Trying to read content of UPDATE_UNUSED_WHILE_PENDING descriptor");
		return;
	}

	if(elemID >= descriptorCount(ds, bindingID)) {
		dlg_trace("elemID out of range");
		return;
	}

	auto& lbinding = ds.layout->bindings[bindingID];
	auto cat = category(lbinding.descriptorType);

	// Setting imageAsBuffer when the descriptor isn't of image type does
	// not make sense
	dlg_assertl(dlg_level_warn, cat == DescriptorCategory::image || !imageAsBuffer);

	if(cat == DescriptorCategory::image) {
		auto& elem = images(ds, bindingID)[elemID];
		if(needsImageView(lbinding.descriptorType)) {
			auto& imgView = elem.imageView;
			dlg_assert(imgView);
			dlg_assert(imgView->img);
			if(imgView->img) {
				// We have to handle the special case where a renderpass
				// attachment is bound in a descriptor set (e.g. as
				// input attachment). In that case, it will be
				// in general layout (via our render pass splitting),
				// not in the layout of the ds.
				auto layout = elem.layout;
				if(info.splitRendering && info.beginRenderPassCmd) {
					dlg_assert(info.beginRenderPassCmd->fb);
					auto& fb = *info.beginRenderPassCmd->fb;
					for(auto* att : fb.attachments) {
						dlg_assert(att->img);
						if(att->img == imgView->img) {
							layout = VK_IMAGE_LAYOUT_GENERAL;
							break;
						}
					}
				} else if(info.splitRendering && info.beginRenderingCmd) {
					// TODO: handle resolveImageLayout
					auto* att = info.beginRenderingCmd->findAttachment(*imgView->img);
					if(att) {
						layout = att->imageLayout;
					}
				}

				// TODO: select exact layer/mip in view range via gui
				auto subres = imgView->ci.subresourceRange;

				if(imageAsBuffer) {
					// we don't ever use that buffer in a submission again
					// so we can ignore queue families
					auto& dstBuf = dst.data.emplace<CopiedImageToBuffer>();
					initAndSampleCopy(dev, cb, dstBuf, *imgView->img, layout,
						subres, {}, imageViews, bufferViews, descriptorSets);

					// TODO: not always needed, only when we copied via
					// compute shader. Make that a return value of initAndSampleCopy?
					info.rebindComputeState = true;
				} else {
					auto& dstImg = dst.data.emplace<CopiedImage>();
					initAndCopy(dev, cb, dstImg, *imgView->img, layout, subres,
						record->queueFamily);
				}
			}
		} else {
			// We shouldn't land here at all, we catch that case when
			// updting the hook in CommandViewer
			dlg_error("Requested descriptor binding copy for sampler");
		}
	} else if(cat == DescriptorCategory::buffer) {
		auto& elem = buffers(ds, bindingID)[elemID];
		dlg_assert(elem.buffer);

		// calculate offset, taking dynamic offset into account
		auto off = elem.offset;
		if(needsDynamicOffset(lbinding.descriptorType)) {
			auto baseOff = lbinding.dynOffset;
			auto dynOffs = dsState.descriptorSets[setID].dynamicOffsets;
			dlg_assert(baseOff + elemID < dynOffs.size());
			off += dynOffs[baseOff + elemID];
		}

		// calculate size
		auto range = elem.range;
		if(range == VK_WHOLE_SIZE) {
			range = elem.buffer->ci.size - off;
		}
		auto size = std::min(maxBufCopySize, range);

		// we don't ever read the buffer from the gfxQueue so we can
		// ignore queueFams here
		auto& dstBuf = dst.data.emplace<OwnBuffer>();
		initAndCopy(dev, cb, dstBuf, 0u, *elem.buffer, off, size, {});
	} else if(cat == DescriptorCategory::accelStruct) {
		auto& elem = accelStructs(ds, bindingID)[elemID];

		using CapturedAccelStruct = CommandHookState::CapturedAccelStruct;
		auto& dstCapture = dst.data.emplace<CapturedAccelStruct>();
		(void) dstCapture;

		accelStructOps.push_back(AccelStructCapture{dstID, elem.accelStruct});
	} else if(cat == DescriptorCategory::bufferView) {
		// TODO: copy as buffer or image? maybe best to copy
		//   as buffer but then create bufferView on our own?
		// auto& dst = state->dsCopy.emplace<CopiedBuffer>();
		// dlg_assert(elem.bufferView->buffer);
		// copyBuffer(dst, elem.bufferView->buffer->handle,
		// 	elem.bufferView->ci.offset, elem.bufferView->ci.range);
		dlg_error("BufferView ds copy unimplemented");
	} else if(cat == DescriptorCategory::inlineUniformBlock) {
		// nothing to copy, data statically bound in state.
		// We shouldn't land here at all, we catch that case when
		// updting the hook in CommandViewer
		dlg_error("Requested descriptor binding copy for inlineUniformBlock");
	} else {
		dlg_error("Unimplemented");
	}
}

void CommandHookRecord::copyAttachment(const Command&, const RecordInfo& info,
		AttachmentType type, unsigned attID,
		CommandHookState::CopiedAttachment& dst) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:copyAttachment");

	if(!info.rpi) {
		dlg_error("copyAttachment but no rpi?!");
		return;
	}

	// NOTE: written in a general way. We might be in a RenderPass
	// or a {Begin, End}Rendering section (i.e. dynamicRendering).
	const RenderPassInstanceState* rpi = info.rpi;
	span<const ImageView* const> attachments;

	switch(type) {
		case AttachmentType::color:
			// written like this since old GCC versions seem to have problems with
			// our span conversion constructor.
			attachments = {rpi->colorAttachments.data(), rpi->colorAttachments.size()};
			break;
		case AttachmentType::input:
			attachments = {rpi->inputAttachments.data(), rpi->inputAttachments.size()};
			break;
		case AttachmentType::depthStencil:
			attachments = {&rpi->depthStencilAttachment, 1u};
			break;
	}

	if(attID >= attachments.size()) {
		dlg_error("copyAttachment ({}, {}} out of range ({})",
			(unsigned) type, attID, attachments.size());
		return;
	}

	auto* imageView = attachments[attID];
	if(!imageView) {
		dlg_warn("copyAttachment on null attachment");
		return;
	}

	dlg_assert(imageView);
	dlg_assert(imageView->img);
	auto* image = imageView->img;

	if(!image) {
		dlg_error("ImageView has no associated image");
		return;
	}

	auto& srcImg = *image;
	VkImageLayout layout {};

	if(info.beginRenderPassCmd && info.splitRendering) {
		layout = VK_IMAGE_LAYOUT_GENERAL; // layout between rp splits, see rp.cpp
	} else if(info.beginRenderingCmd && info.splitRendering) {
		switch(type) {
			case AttachmentType::color:
				dlg_assert(attID < info.beginRenderingCmd->colorAttachments.size());
				layout = info.beginRenderingCmd->colorAttachments[attID].imageLayout;
				break;
			case AttachmentType::depthStencil:
				dlg_assert(attID == 0u);
				layout = info.beginRenderingCmd->depthAttachment.imageLayout;
				break;
			case AttachmentType::input:
				dlg_error("unreachable");
				return;
		}
	}

	// TODO: select exact layer/mip in view range via gui
	auto& subres = imageView->ci.subresourceRange;
	initAndCopy(dev, cb, dst.data, srcImg, layout, subres,
		record->queueFamily);
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

void CommandHookRecord::copyTransfer(Command& bcmd, RecordInfo& info, bool isBefore) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:copyTransfer");

	struct CopyImage {
		Image* src {};
		VkImageLayout layout {};
		VkImageSubresourceRange subres {};
	};

	struct CopyBuffer {
		Buffer* buf {};
		VkDeviceSize offset {};
		VkDeviceSize size {};
	};

	auto idx = info.ops.transferIdx;
	if(isBefore ? info.ops.copyTransferSrcBefore : info.ops.copyTransferSrcAfter) {
		std::optional<CopyImage> img;
		std::optional<CopyBuffer> buf;

		if(auto* cmd = commandCast<const CopyImageCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->copies[idx].srcSubresource)};
		} else if(auto* cmd = commandCast<const BlitImageCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->blits[idx].srcSubresource)};
		} else if(auto* cmd = commandCast<const CopyImageToBufferCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->copies[idx].imageSubresource)};
		} else if(auto* cmd = commandCast<const ResolveImageCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->regions[idx].srcSubresource)};
		} else if(auto* cmd = commandCast<const CopyBufferCmd*>(&bcmd); cmd) {
			auto [offset, size] = minMaxInterval({{cmd->regions[idx]}}, true);
			buf = {cmd->src, offset, size};
		} else if(auto* cmd = commandCast<const CopyBufferToImageCmd*>(&bcmd); cmd) {
			auto texelSize = FormatTexelSize(cmd->dst->ci.format);
			auto [offset, size] = minMaxInterval({{cmd->copies[idx]}}, texelSize);
			buf = {cmd->src, offset, size};
		}

		auto& toWrite = isBefore ? state->transferSrcBefore : state->transferSrcAfter;

		dlg_assert(img || buf);
		if(img) {
			auto [src, layout, subres] = *img;
			initAndCopy(dev, cb, toWrite.img, *src,
				layout, subres, record->queueFamily);
		} else if(buf) {
			auto [src, offset, size] = *buf;
			if(copyFullTransferBuffer) {
				offset = 0u;
				size = src->ci.size;
			}

			// we don't ever read the buffer from the gfxQueue so we can
			// ignore queueFams here
			initAndCopy(dev, cb, toWrite.buf, 0u, *src, offset, size, {});
		}
	}

	if(isBefore ? info.ops.copyTransferDstBefore : info.ops.copyTransferDstAfter) {
		std::optional<CopyImage> img;
		std::optional<CopyBuffer> buf;

		if(auto* cmd = commandCast<const CopyImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->copies[idx].dstSubresource)};
		} else if(auto* cmd = commandCast<const BlitImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->blits[idx].dstSubresource)};
		} else if(auto* cmd = commandCast<const CopyBufferToImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->copies[idx].imageSubresource)};
		} else if(auto* cmd = commandCast<const ResolveImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->regions[idx].dstSubresource)};
		} else if(auto* cmd = commandCast<const ClearColorImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, cmd->ranges[idx]};
		} else if(auto* cmd = commandCast<const ClearDepthStencilImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, cmd->ranges[idx]};
		} else if(auto* cmd = commandCast<const ClearAttachmentCmd*>(&bcmd)) {
			dlg_assert(info.beginRenderPassCmd->rp && info.beginRenderPassCmd->fb);
			auto& rp = *info.beginRenderPassCmd->rp;
			auto& fb = *info.beginRenderPassCmd->fb;

			// TODO: support showing multiple cleared attachments in gui,
			//   allowing to select here which one is copied.
			auto& clearAtt = cmd->attachments[idx];
			u32 attID = clearAtt.colorAttachment;
			if(clearAtt.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
				// TODO: I guess other values are allowed here as well, fix it
				dlg_assertm_or(clearAtt.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT,
					return, "Only depth and color copies supported");

				auto& subpass = rp.desc.subpasses[info.hookedSubpass];
				dlg_assert(subpass.pDepthStencilAttachment);
				auto& depthStencil = *subpass.pDepthStencilAttachment;
				attID = depthStencil.attachment;
			}

			dlg_assert(fb.attachments.size() > attID);
			dlg_assert(fb.attachments[attID] && fb.attachments[attID]->img);
			auto& imgView = *fb.attachments[attID];
			auto& src = *imgView.img;

			// image must be in general layout because we are just between
			// the split render passes
			img = {&src, VK_IMAGE_LAYOUT_GENERAL, imgView.ci.subresourceRange};
		} else if(auto* cmd = commandCast<const CopyBufferCmd*>(&bcmd); cmd) {
			auto [offset, size] = minMaxInterval({{cmd->regions[idx]}}, false);
			buf = {cmd->dst, offset, size};
		} else if(auto* cmd = commandCast<const CopyImageToBufferCmd*>(&bcmd); cmd) {
			auto texelSize = FormatTexelSize(cmd->src->ci.format);
			auto [offset, size] = minMaxInterval({{cmd->copies[idx]}}, texelSize);
			buf = {cmd->dst, offset, size};
		} else if(auto* cmd = commandCast<const FillBufferCmd*>(&bcmd); cmd) {
			buf = {cmd->dst, cmd->offset, cmd->size};
		} else if(auto* cmd = commandCast<const UpdateBufferCmd*>(&bcmd); cmd) {
			buf = {cmd->dst, cmd->offset, cmd->data.size()};
		}

		auto& toWrite = isBefore ? state->transferDstBefore : state->transferDstAfter;
		dlg_assert(img || buf);
		if(img) {
			auto [src, layout, subres] = *img;
			initAndCopy(dev, cb, toWrite.img, *src,
				layout, subres, record->queueFamily);
		} else if(buf) {
			auto [src, offset, size] = *buf;
			if(copyFullTransferBuffer) {
				offset = 0u;
				size = src->ci.size;
			}

			// we don't ever read the buffer from the gfxQueue so we can
			// ignore queueFams here
			initAndCopy(dev, cb, toWrite.buf, 0u, *src, offset, size, {});
		}
	}
}

void CommandHookRecord::beforeDstOutsideRp(Command& bcmd, RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:beforeDstOutsideRp");

	if(info.splitRendering) {
		// TODO: kinda hacky, can be improved. But we definitely need a general barrier here,
		// between the render passes to make sure the first render pass really
		// has finished (with *everything*, not just the stuff we are interested
		// in here) before we start the second one.
		// NOTE: memory_write | memory_read *should* be enough here, they cover everything else.
		// But we noticed this to make a difference on some drivers (e.g. AMD on windows)
		auto access =
			VK_ACCESS_MEMORY_WRITE_BIT |
			VK_ACCESS_MEMORY_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;

		VkMemoryBarrier memBarrier {};
		memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memBarrier.srcAccessMask = access;
		memBarrier.dstAccessMask = access;

		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 1, &memBarrier, 0, nullptr, 0, nullptr);
	}

	// indirect copy
	if(info.ops.copyIndirectCmd) {
		DebugLabel lbl(dev, cb, "vil:copyInderectCmd");

		// we don't ever read the buffer from the gfxQueue so we can
		// ignore queueFams here
		if(auto* cmd = commandCast<DrawIndirectCmd*>(&bcmd)) {
			VkDeviceSize stride = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd->stride ? cmd->stride : stride;
			auto dstSize = cmd->drawCount * stride;
			dlg_assert(cmd->buffer);
			initAndCopy(dev, cb, state->indirectCopy,  0u,
				*cmd->buffer, cmd->offset, dstSize, {});
		} else if(auto* cmd = commandCast<DispatchIndirectCmd*>(&bcmd)) {
			dlg_assert(cmd->buffer);
			auto size = sizeof(VkDispatchIndirectCommand);
			initAndCopy(dev, cb, state->indirectCopy, 0u,
				*cmd->buffer, cmd->offset, size, {});
		} else if(auto* cmd = commandCast<DrawIndirectCountCmd*>(&bcmd)) {
			dlg_assert(cmd->buffer && cmd->countBuffer);

			auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto size = 4 + cmd->maxDrawCount * cmdSize;
			state->indirectCopy.ensure(dev, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

			// copy count
			performCopy(dev, cb, *cmd->countBuffer, cmd->countBufferOffset,
				state->indirectCopy, 0u, 4u);
			// copy commands
			// NOTE: using an indirect-transfer-emulation approach (see
			// below, same problem as for indirect draw vertex/index bufs)
			// we could avoid copying too much data here. Likely not worth
			// it here though unless application pass *huge* maxDrawCount
			// values (which they shouldn't).
			performCopy(dev, cb, *cmd->buffer, cmd->offset,
				state->indirectCopy, 4u, cmd->maxDrawCount * cmdSize);
		} else if(auto* cmd = commandCast<TraceRaysIndirectCmd*>(&bcmd)) {
			auto size = sizeof(VkTraceRaysIndirectCommandKHR);
			initAndCopy(dev, cb, state->indirectCopy, cmd->indirectDeviceAddress,
				size, {});
		} else {
			dlg_error("Unsupported indirect command");
		}
	}

	// attachments
	for(auto [i, ac] : enumerate(info.ops.attachmentCopies)) {
		if(ac.before) {
			state->copiedAttachments[i].op = ac;
			copyAttachment(bcmd, info, ac.type, ac.id, state->copiedAttachments[i]);
		}
	}

	// descriptor state
	for(auto [i, dc] : enumerate(info.ops.descriptorCopies)) {
		if(dc.before) {
			IntrusivePtr<DescriptorSetCow> tmpCow; // TODO: due to dsState removal
			copyDs(bcmd, info, dc, i, state->copiedDescriptors[i], tmpCow);
		}
	}

	// We might use the vertex/index buffer copies when rendering the ui
	// later on so we have to care about queue families
	auto queueFams = combineQueueFamilies({{record->queueFamily, dev.gfxQueue->family}});

	// PERF: we could support tighter buffer bounds for indirect/indexed draw
	// calls. See node 1749 for a sketch using a couple of compute shaders,
	// basically emulating an indirect transfer.
	// PERF: for non-indexed/non-indirect draw calls we know the exact
	// sizes of vertex/index buffers to copy, we could use that.
	auto maxVertIndSize = maxBufCopySize;

	const bool isDraw = bcmd.category() == CommandCategory::draw;
	dlg_assert(!info.ops.copyVertexBuffers || isDraw);
	if(info.ops.copyVertexBuffers && isDraw) {
		DebugLabel lbl(dev, cb, "vil:copyVertexBuffers");

		auto* drawCmd = deriveCast<DrawCmdBase*>(&bcmd);
		for(auto& vertbuf : drawCmd->state->vertices) {
			auto& dst = state->vertexBufCopies.emplace_back();
			if(!vertbuf.buffer) {
				continue;
			}

			auto size = std::min(maxVertIndSize, vertbuf.buffer->ci.size - vertbuf.offset);
			initAndCopy(dev, cb, dst, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				*vertbuf.buffer, vertbuf.offset, size, queueFams);
		}
	}

	dlg_assert(!info.ops.copyIndexBuffers || isDraw);
	if(info.ops.copyIndexBuffers && isDraw) {
		DebugLabel lbl(dev, cb, "vil:copyIndexBuffers");

		auto* drawCmd = deriveCast<DrawCmdBase*>(&bcmd);
		auto& inds = drawCmd->state->indices;
		if(inds.buffer) {
			auto size = std::min(maxVertIndSize, inds.buffer->ci.size - inds.offset);
			initAndCopy(dev, cb, state->indexBufCopy, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				*inds.buffer, inds.offset, size, queueFams);
		}
	}

	// transfer
	if(info.ops.copyTransferSrcBefore || info.ops.copyTransferDstBefore) {
		copyTransfer(bcmd, info, true);
	}
}

void CommandHookRecord::afterDstOutsideRp(Command& bcmd, RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:afterDsOutsideRp");

	if(info.splitRendering) {
		// TODO: kinda hacky, can be improved. But we definitely need a general barrier here,
		// between the render passes to make sure the second render pass really
		// has finished (with *everything*, not just the stuff we are interested
		// in here) before we start the third one.
		// NOTE: memory_write | memory_read *should* be enough here, they cover everything else.
		// But we noticed this to make a difference on some drivers (e.g. AMD on windows)
		auto access =
			VK_ACCESS_MEMORY_WRITE_BIT |
			VK_ACCESS_MEMORY_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;

		VkMemoryBarrier memBarrier {};
		memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memBarrier.srcAccessMask = access;
		memBarrier.dstAccessMask = access;

		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 1, &memBarrier, 0, nullptr, 0, nullptr);
	}

	// attachments
	for(auto [i, ac] : enumerate(info.ops.attachmentCopies)) {
		if(!ac.before) {
			state->copiedAttachments[i].op = ac;
			copyAttachment(bcmd, info, ac.type, ac.id, state->copiedAttachments[i]);
		}
	}

	// descriptor state
	for(auto [i, dc] : enumerate(info.ops.descriptorCopies)) {
		if(!dc.before) {
			IntrusivePtr<DescriptorSetCow> tmpCow; // TODO: due to dsState removal
			copyDs(bcmd, info, dc, i, state->copiedDescriptors[i], tmpCow);
		}
	}

	// transfer
	if(info.ops.copyTransferSrcAfter || info.ops.copyTransferDstAfter) {
		copyTransfer(bcmd, info, false);
	}
}

// see accelStructVertices.comp, must match
static const u32 vertTypeRG32f = 1u;
static const u32 vertTypeRGB32f = 2u;
static const u32 vertTypeRGBA32f = 3u;

static const u32 vertTypeRG16f = 4u;
static const u32 vertTypeRGBA16f = 5u;

static const u32 vertTypeRG16s = 6u;
static const u32 vertTypeRGBA16s = 7u;

u32 getVertType(VkFormat fmt) {
	switch(fmt) {
		case VK_FORMAT_R32G32_SFLOAT: return vertTypeRG32f;
		case VK_FORMAT_R32G32B32_SFLOAT: return vertTypeRGB32f;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return vertTypeRGBA32f;
		case VK_FORMAT_R16G16_SNORM: return vertTypeRG16s;
		case VK_FORMAT_R16G16B16A16_SNORM: return vertTypeRGBA16s;
		case VK_FORMAT_R16G16_SFLOAT: return vertTypeRG16f;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return vertTypeRGBA16f;
		default:
			dlg_error("Unsupported AccelerationStructure vertex format");
			return 0u;
	}
}

void CommandHookRecord::hookBefore(const BuildAccelStructsCmd& cmd) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:beforeBuildAccelStructs");

	auto& cmdHook = *dev.commandHook;

	auto& ops = accelStructOps.emplace_back();
	auto& build = ops.emplace<AccelStructBuild>();
	build.command = &cmd;

	// TODO 1. Make sure all data has been written via memory barrier.
	// The application might have set barriers that don't cover
	// our case of reading data in compute shaders.
	// Don't do the various small barriers below (and in performCopy)

	// 2. initialize data
	for(auto i = 0u; i < cmd.buildInfos.size(); ++i) {
		auto& srcBuildInfo = cmd.buildInfos[i];
		auto& rangeInfos = cmd.buildRangeInfos[i];
		auto& accelStruct = *cmd.dsts[i];

		auto& dst = build.builds.emplace_back();
		// safe to just reference them here, record will stay alive at least
		// until we read it again.
		dst.dst = &accelStruct;

		// init AccelStructState
		dlg_assert(cmd.buildRangeInfos[i].size() == srcBuildInfo.geometryCount);

		dst.state = createState(*dst.dst, srcBuildInfo, cmd.buildRangeInfos[i].data());
		auto& state = *dst.state;

		auto& dstBuffer = state.buffer;
		dlg_assert(dstBuffer.size);

		VkBufferDeviceAddressInfo addrInfo {};
		addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		addrInfo.buffer = dstBuffer.buf;
		auto dstAddress = dev.dispatch.GetBufferDeviceAddress(dev.handle, &addrInfo);
		dlg_assert(dstAddress);

		auto dstOff = 0u;
		for(auto g = 0u; g < srcBuildInfo.geometryCount; ++g) {
			auto& range = rangeInfos[g];
			auto& srcGeom = srcBuildInfo.pGeometries ?
				srcBuildInfo.pGeometries[g] : *srcBuildInfo.ppGeometries[g];

			if(srcGeom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
				dlg_error("TODO: need shader");
			} else if(srcGeom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
				auto& srcTris = srcGeom.geometry.triangles;

				// copy vertices
				dlg_assert(cmdHook.accelStructVertCopy_);

				// make sure we can read it via copy
				auto& vertBuf = bufferAtLocked(dev, srcTris.vertexData.deviceAddress);
				dlg_assert(vertBuf.deviceAddress);

				VkBufferMemoryBarrier barriers[2] = {};
				barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
				barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[0].buffer = vertBuf.handle;
				barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
				barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				barriers[0].size = srcTris.maxVertex * srcTris.vertexStride;
				barriers[0].offset = srcTris.vertexData.deviceAddress - vertBuf.deviceAddress;

				auto nbarriers = 1u;
				if(srcTris.indexType != VK_INDEX_TYPE_NONE_KHR) {
					dlg_assert(srcTris.indexData.deviceAddress);
					auto& indBuf = bufferAtLocked(dev, srcTris.indexData.deviceAddress);
					dlg_assert(indBuf.deviceAddress);

					barriers[1] = barriers[0];
					barriers[1].buffer = indBuf.handle;
					barriers[1].size = indexSize(srcTris.indexType) * range.primitiveCount * 3;
					barriers[1].offset = srcTris.indexData.deviceAddress - indBuf.deviceAddress;
					++nbarriers;
				}

				dev.dispatch.CmdPipelineBarrier(cb,
					VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
					0u, nullptr, nbarriers, barriers, 0u, nullptr);

				// TODO: we can't assume this. But currently need it for
				// the shader, would have to do work on raw bytes otherwise
				// which is a pain.
				dlg_assertm(srcTris.vertexStride % 4u == 0u,
					"Building acceleration structures with vertexStride % 4 != 0 not implemented");
				dlg_assert(srcTris.vertexStride >= FormatElementSize(srcTris.vertexFormat));

				dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
					cmdHook.accelStructVertCopy_);

				// sizes and strides are always multiples of 4
				struct {
					u64 indPtr;
					u64 vertPtr;
					u64 transformPtr;
					u64 dstPtr;
					u32 count;
					u32 indexSize;
					u32 vertType;
					u32 vertStride;
				} pcr;

				pcr.indPtr = srcTris.indexData.deviceAddress;
				pcr.vertPtr = srcTris.vertexData.deviceAddress;
				pcr.transformPtr = srcTris.transformData.deviceAddress;
				pcr.dstPtr = dstAddress + dstOff;
				pcr.indexSize = indexSize(srcTris.indexType);
				pcr.vertStride = srcTris.vertexStride / 4u;
				pcr.vertType = getVertType(srcTris.vertexFormat);
				pcr.count = 3 * range.primitiveCount;

				pcr.vertPtr += range.firstVertex * srcTris.vertexStride;
				if(srcTris.indexType == VK_INDEX_TYPE_NONE_KHR) {
					pcr.vertPtr += range.primitiveOffset;
				} else {
					dlg_assert(pcr.indPtr);
					pcr.indPtr += range.primitiveOffset;
				}

				if(pcr.transformPtr) {
					pcr.transformPtr += range.transformOffset;
				}

				dev.dispatch.CmdPushConstants(cb, cmdHook.accelStructPipeLayout_,
					VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(pcr), &pcr);

				auto gx = ceilDivide(3 * range.primitiveCount, 64u);
				dev.dispatch.CmdDispatch(cb, gx, 1u, 1u);

				dstOff += range.primitiveCount * sizeof(AccelTriangles::Triangle);
			} else if(srcGeom.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
				// TODO: resolve indirection via custom compute shader
				auto& inis = srcGeom.geometry.instances;
				dlg_assertm(!inis.arrayOfPointers,
					"TODO: arrayOfPointers not supported yet");

				auto srcAddr = inis.data.deviceAddress;
				srcAddr += range.primitiveOffset;
				auto size = sizeof(VkAccelerationStructureInstanceKHR) * range.primitiveCount;
				performCopy(dev, cb, srcAddr, dstBuffer, dstOff, size);

				dstOff += size;
			} else {
				dlg_fatal("invalid geometry type {}", srcGeom.geometryType);
			}
		}
	}
}

void CommandHookRecord::hookBefore(const BuildAccelStructsIndirectCmd& cmd) {
	// TODO: implement indirect copy concept
	(void) cmd;
	dlg_error("TODO: implement support for copying BuildAccelStructsIndirectCmd data");
}

void CommandHookRecord::finish() noexcept {
	// NOTE: We don't do this since we can assume the record to remain
	// valid until all submissions are finished. We can assume it to
	// be valid throughout the entire lifetime of *this.
	// record = nullptr;

	// Keep alive when there still a pending submission.
	// It will delete this record then instead.
	if(!writer) {
		delete this;
	} else {
		// The only reason we might land here is when the record
		// was invalidated.
		dlg_assert(!hook);
	}
}

} // namespace vil
