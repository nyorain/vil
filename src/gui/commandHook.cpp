#include <gui/commandHook.hpp>
#include <device.hpp>
#include <ds.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <pipe.hpp>
#include <rp.hpp>
#include <accelStruct.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <gui/gui.hpp>
#include <command/desc.hpp>
#include <command/commands.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <vk/format_utils.h>
#include <accelStructVertices.comp.spv.h>

// TODO: instead of doing memory barrier per-resource when copying to
//   our readback buffers, we should probably do just do general memory
//   barriers.

namespace vil {

// util
const DescriptorState* getDsState(const Command& cmd) {
	switch(cmd.type()) {
		case CommandType::draw:
			return &deriveCast<const DrawCmdBase*>(&cmd)->state;
		case CommandType::dispatch:
			return &deriveCast<const DispatchCmdBase*>(&cmd)->state;
		case CommandType::traceRays:
			return &deriveCast<const TraceRaysCmdBase*>(&cmd)->state;
		default:
			return nullptr;
	}
}

// Expects a and to have the same layout.
// If the descriptor at (bindingID, elemID) needs to be copied by CommandHook,
// returns whether its the same in a and b.
bool copyableDescriptorSame(const DescriptorSetState& a, const DescriptorSetState& b,
		unsigned bindingID, unsigned elemID) {
	if(&a == &b) {
		return true;
	}

	dlg_assert(a.layout == b.layout);
	dlg_assert(bindingID < a.layout->bindings.size());
	dlg_assert(elemID < descriptorCount(a, bindingID));

	auto& lbinding = a.layout->bindings[bindingID];
	auto cat = category(lbinding.descriptorType);
	if(cat == DescriptorCategory::image) {
		return images(a, bindingID)[elemID] == images(b, bindingID)[elemID];
	} else if(cat == DescriptorCategory::buffer) {
		return buffers(a, bindingID)[elemID] == buffers(b, bindingID)[elemID];
	} else if(cat == DescriptorCategory::bufferView) {
		return bufferViews(a, bindingID)[elemID] == bufferViews(b, bindingID)[elemID];
	} else if(cat == DescriptorCategory::accelStruct) {
		// TODO: do we need to copy acceleration structues? Might be hard
		// to do correctly; should use copy-on-write. Not sure if worth it at all.
		return true;
	} else if(cat == DescriptorCategory::inlineUniformBlock) {
		return true;
	}

	dlg_error("Invalid descriptor type");
	return false;
}

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

void invalidate(CommandHookRecord& rec) {
	rec.hook = nullptr; // notify the record that it's no longer needed
	auto it = find(rec.record->hookRecords, &rec);
	dlg_assert(it != rec.record->hookRecords.end());

	// CommandRecord::Hook is a FinishPtr.
	// This will delete our record hook if there are no pending
	// submissions of it left. See CommandHookRecord::finish
	rec.record->hookRecords.erase(it);
}

// CommandHook
CommandHook::CommandHook(Device& dev) {
	dev_ = &dev;
	if(hasAppExt(dev, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
		initAccelStructCopy(dev);
	}
}

CommandHook::~CommandHook() {
	invalidateRecordings();

	auto& dev = *dev_;
	dev.dispatch.DestroyPipelineLayout(dev.handle, accelStructPipeLayout_, nullptr);
	dev.dispatch.DestroyPipeline(dev.handle, accelStructVertCopy_, nullptr);
}

void CommandHook::initAccelStructCopy(Device& dev) {
	// We just allocate the full push constant range that all implementations
	// must support.
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcrs[0].offset = 0;
	pcrs[0].size = 128; // needed e.g. for vertex viewer pipeline

	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr,
		&accelStructPipeLayout_));
	nameHandle(dev, accelStructPipeLayout_, "CommandHook:accelStructPipeLayout");

	// init pipeline
	VkShaderModule mod {};

	VkShaderModuleCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	sci.codeSize = sizeof(accelStructVertices_comp_spv_data);
	sci.pCode = accelStructVertices_comp_spv_data;

	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &sci, nullptr, &mod));

	// create pipeline
	VkComputePipelineCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.layout = accelStructPipeLayout_;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = mod;
	cpi.stage.pName = "main";

	VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, VK_NULL_HANDLE,
		1u, &cpi, nullptr, &accelStructVertCopy_));
	nameHandle(dev, accelStructVertCopy_, "CommandHook:accelStructVertCopy");

	dev.dispatch.DestroyShaderModule(dev.handle, mod, nullptr);
}

VkCommandBuffer CommandHook::hook(CommandBuffer& hooked,
		Submission& subm, std::unique_ptr<CommandHookSubmission>& data) {
	dlg_assert(hooked.state() == CommandBuffer::State::executable);
	ZoneScoped;

	auto& dev = *hooked.dev;
	auto& record = nonNull(hooked.lastRecordLocked());

	// Check whether we should attempt to hook this particular record
	bool hookNeededForCmd = true;
	const bool validTarget =
		&record == target.record ||
		&hooked == target.cb ||
		target.all;

	if(!validTarget || !record_ || hierachy_.empty() || !record.commands) {
		hookNeededForCmd = false;
	}

	// When there is no gui viewing the submissions at the moment, we don't
	// need/want to hook the submission.
	if(!dev.gui || !dev.gui->visible || freeze ||
			dev.gui->activeTab() != Gui::Tab::commandBuffer) {
		hookNeededForCmd = false;
	}

	// Even when we aren't interested in any command in the record, we have
	// to hook it when it builds acceleration structures.
	if(!hookNeededForCmd && !record.buildsAccelStructs) {
		return hooked.handle();
	}

	// Check if there already is a valid CommandHookRecord we can use.
	CommandHookRecord* foundHookRecord {};
	CommandHookRecord* foundCompleted = nullptr;
	auto foundCompletedIt = completed.end();
	auto completedCount = 0u;

	for(auto& hookRecord : record.hookRecords) {
		// we can't use this record since it didn't hook a command and
		// was just use for accelStruct data copying
		if(hookNeededForCmd == hookRecord->hcommand.empty()) {
			continue;
		}

		// the record is currently pending on the device
		if(hookRecord->writer) {
			continue;
		}

		// the record has completed, its state in our completed list
		auto completedIt = find_if(this->completed, [&](const CompletedHook& completed) {
			return completed.state == hookRecord->state;
		});
		if(completedIt != completed.end()) {
			++completedCount;
			if(!foundCompleted) {
				foundCompletedIt = completedIt;
				foundCompleted = hookRecord.get();
			}

			continue;
		}

		foundHookRecord = hookRecord.get();
		break;
	}

	// If there are already 2 versions for this record in our completed
	// list, we can just take one of them (preferrably the older one)
	// and reuse its state.
	if(!foundHookRecord && completedCount > 2) {
		dlg_assert(foundCompletedIt != completed.end());
		dlg_assert(foundCompleted);
		this->completed.erase(foundCompletedIt);
		foundHookRecord = foundCompleted;
	}

	if(foundHookRecord) {
		bool usable = true;

		if(hookNeededForCmd) {
			dlg_check({
				// Before calling find, we need to unset the invalidated handles from the
				// commands in hierachy_, find relies on all of them being valid.
				replaceInvalidatedLocked(nonNull(record_));

				auto findRes = find(record.commands, hierachy_, dsState_);
				dlg_assert(std::equal(
					foundHookRecord->hcommand.begin(), foundHookRecord->hcommand.end(),
					findRes.hierachy.begin(), findRes.hierachy.end()));
				// NOTE: I guess we can't really rely on this. Just always call
				// findRes? But i'm not even sure this is important. Could we ever
				// suddenly want to hook a different command in the same record
				// without the selection changing (in which case the hooked record
				// would have been invalidated). In question, just remove this
				// assert. It's useful for debugging though; was previously
				// used to uncover new find/matching issues.
				// dlg_assertm(std::abs(findRes.match - foundHookRecord->match) < 0.1,
				// 	"{} -> {}", foundHookRecord->match, findRes.match);
			});

			dlg_assert(foundHookRecord->hookCounter == counter_);
			dlg_assert(foundHookRecord->state);
		}

		dlg_assert(foundHookRecord->hook == this);
		dlg_assert(!foundHookRecord->writer);

		// Not possible to reuse the hook-recorded cb when the command
		// buffer uses any update_after_bind descriptors that changed.
		// We therefore compare them.
		dlg_assert(!copyDS || foundHookRecord->dsState);
		if(copyDS && foundHookRecord->dsState && hookNeededForCmd) {
			auto [setID, bindingID, elemID, _] = *copyDS;

			dlg_assert(!foundHookRecord->hcommand.empty());
			auto* cmd = foundHookRecord->hcommand.back();
			const DescriptorState* dsState = getDsState(*cmd);

			auto& dsSnapshot = record.lastDescriptorState;
			dlg_assert(setID < dsState->descriptorSets.size());
			auto it = dsSnapshot.states.find(dsState->descriptorSets[setID].ds);

			dlg_assert(it != dsSnapshot.states.end());
			auto& currDS = nonNull(it->second);

			if(!copyableDescriptorSame(currDS, *foundHookRecord->dsState, bindingID, elemID)) {
				usable = false;
				invalidate(*foundHookRecord);
				foundHookRecord = nullptr;
			}
		}

		if(usable) {
			data.reset(new CommandHookSubmission(*foundHookRecord, subm));
			return foundHookRecord->cb;
		}
	}

	FindResult findRes {};
	if(hookNeededForCmd) {
		// Before calling find, we need to unset the invalidated handles from the
		// commands in hierachy_, find relies on all of them being valid.
		replaceInvalidatedLocked(nonNull(record_));

		findRes = find(record.commands, hierachy_, dsState_);
		if(findRes.hierachy.empty()) {
			// Can't find the command we are looking for in this record
			return hooked.handle();
		}

		dlg_assert(findRes.hierachy.size() == hierachy_.size());
	}

	dlg_assertlm(dlg_level_warn, record.hookRecords.size() < 8,
		"Alarmingly high number of hooks for a single record");

	auto hook = new CommandHookRecord(*this, record, std::move(findRes.hierachy));
	hook->match = findRes.match;
	record.hookRecords.emplace_back(hook);

	data.reset(new CommandHookSubmission(*hook, subm));

	return hook->cb;
}

void CommandHook::desc(IntrusivePtr<CommandRecord> rec,
		std::vector<const Command*> hierachy,
		CommandDescriptorSnapshot dsState, bool invalidate) {
	dlg_assert(bool(rec) == !hierachy.empty());

	record_ = std::move(rec);
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

		// we don't want to invalidate recordings that didn't hook a command
		// and were only done for accelStruct builddata copying
		if(!rec->hcommand.empty()) {
			invalidate(*rec);
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

// record
CommandHookRecord::CommandHookRecord(CommandHook& xhook,
	CommandRecord& xrecord, std::vector<const Command*> hooked) :
		hook(&xhook), record(&xrecord), hcommand(std::move(hooked)) {

	++DebugStats::get().aliveHookRecords;

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
	// we can never submit the cb simulataneously anyways, see CommandHook
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(dev.dispatch.BeginCommandBuffer(this->cb, &cbbi));

	// initial cmd stuff
	if(hook->queryTime) {
		dev.dispatch.CmdResetQueryPool(cb, queryPool, 0, 2);
	}

	unsigned maxHookLevel {};
	info.maxHookLevel = &maxHookLevel;

	ZoneScopedN("HookRecord");
	this->hookRecord(record->commands, info);

	VK_CHECK(dev.dispatch.EndCommandBuffer(this->cb));

	if(!hcommand.empty()) {
		dlg_assert(maxHookLevel >= hcommand.size() - 1);
		dlg_assert(hcommand.back()->children() || maxHookLevel == hcommand.size() - 1);
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
	if(hcommand.empty()) {
		return;
	}

	auto& dev = *record->dev;
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
		 hook->copyIndirectCmd ||
		 (hook->copyTransferDst && dynamic_cast<const ClearAttachmentCmd*>(hcommand.back())));

	if(info.splitRenderPass) {
		auto& rp = *info.beginRenderPassCmd->rp;

		// TODO: we could likely just directly support this
		if(hasChain(rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
			state->errorMessage = "Splitting multiview renderpass not implemented";
			dlg_trace(state->errorMessage);
			info.splitRenderPass = false;
		}
	}

	if(info.splitRenderPass) {
		auto& desc = info.beginRenderPassCmd->rp->desc;

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

		auto numSubpasses = info.beginRenderPassCmd->rp->desc.subpasses.size();
		for(auto i = info.hookedSubpass; i + 1 < numSubpasses; ++i) {
			// TODO: missing potential forward of pNext chain here
			// Subpass contents irrelevant here.
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
		if(drawCmd->state.pipe->xfbPatch && hook->copyXfb) {
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

		// check if command needs additional, manual hook
		if(cmd->type() == CommandType::buildAccelStruct) {
			auto* basCmd = dynamic_cast<BuildAccelStructsCmd*>(cmd);
			auto* basCmdIndirect = dynamic_cast<BuildAccelStructsCmd*>(cmd);
			dlg_assert(basCmd || basCmdIndirect);

			if(basCmd) {
				hookBefore(*basCmd);
			} else if(basCmdIndirect) {
				hookBefore(*basCmdIndirect);
			}
		}

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

				// we never actually record CmdExecuteCommands when hook-recording,
				// so always pass inline here.
				auto subpassBeginInfo = info.beginRenderPassCmd->subpassBeginInfo;
				subpassBeginInfo.contents = VK_SUBPASS_CONTENTS_INLINE;

				if(beginRpCmd->subpassBeginInfo.pNext) {
					auto beginRp2 = dev.dispatch.CmdBeginRenderPass2;
					dlg_assert(beginRp2);
					beginRp2(cb, &rpBeginInfo, &subpassBeginInfo);
				} else {
					dev.dispatch.CmdBeginRenderPass(cb, &rpBeginInfo, subpassBeginInfo.contents);
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

void performCopy(Device& dev, VkCommandBuffer cb, VkDeviceAddress srcPtr,
		OwnBuffer& dst, VkDeviceSize dstOffset, VkDeviceSize size) {
	auto& srcBuf = bufferAtLocked(dev, srcPtr);
	auto srcOff = srcPtr - srcBuf.deviceAddress;
	performCopy(dev, cb, srcBuf, srcOff, dst, dstOffset, size);
}

void initAndCopy(Device& dev, VkCommandBuffer cb, OwnBuffer& dst,
		VkBufferUsageFlags addFlags, Buffer& src,
		VkDeviceSize offset, VkDeviceSize size) {
	addFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	dst.ensure(dev, size, addFlags);
	performCopy(dev, cb, src, offset, dst, 0, size);
}

void CommandHookRecord::copyDs(Command& bcmd, const RecordInfo& info) {
	auto& dev = *record->dev;

	const DescriptorState* dsState = getDsState(bcmd);
	if(!dsState) {
		state->errorMessage = "Unsupported descriptor command";
		dlg_error("{}", state->errorMessage);
		hook->copyDS = {};
		return;
	}

	auto [setID, bindingID, elemID, _] = *hook->copyDS;

	// NOTE: we have to check for correct sizes here since the
	// actual command might have changed (for an updated record)
	// and the selected one not valid anymore.
	if(setID >= dsState->descriptorSets.size()) {
		dlg_trace("setID out of range");
		hook->copyDS = {};
		return;
	}

	auto& dsSnapshot = record->lastDescriptorState;

	auto it = dsSnapshot.states.find(dsState->descriptorSets[setID].ds);
	if(it == dsSnapshot.states.end()) {
		dlg_error("Could not find descriptor in snapshot??");
		hook->copyDS = {};
		return;
	}

	auto& ds = nonNull(it->second);
	if(bindingID >= ds.layout->bindings.size()) {
		dlg_trace("bindingID out of range");
		hook->copyDS = {};
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
		state->errorMessage = "Can't read UPDATE_UNUSED_WHILE_PENDING descriptors at the moment";
		hook->copyDS = {};
		return;
	}

	if(elemID >= descriptorCount(*it->second, bindingID)) {
		dlg_trace("elemID out of range");
		hook->copyDS = {};
		return;
	}

	this->dsState = it->second;

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
			// TODO we should not land here at all! Check state in
			// cb gui before registring hook. Don't register a hook
			// just to find out *here* that we don't need it
			state->errorMessage = "Just a sampler bound";
			dlg_error(state->errorMessage);
		}
	} else if(cat == DescriptorCategory::buffer) {
		auto& elem = buffers(*it->second, bindingID)[elemID];
		dlg_assert(elem.buffer);

		// calculate offset, taking dynamic offset into account
		auto off = elem.offset;
		if(needsDynamicOffset(lbinding.descriptorType)) {
			auto baseOff = lbinding.dynOffset;
			auto dynOffs = dsState->descriptorSets[setID].dynamicOffsets;
			dlg_assert(baseOff + elemID < dynOffs.size());
			off += dynOffs[baseOff + elemID];
		}

		// calculate size
		auto range = elem.range;
		if(range == VK_WHOLE_SIZE) {
			range = elem.buffer->ci.size - elem.offset;
		}
		auto size = std::min(maxBufCopySize, range);

		auto& dst = state->dsCopy.emplace<OwnBuffer>();
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
	} else if(cat == DescriptorCategory::inlineUniformBlock) {
		// nothing to copy, data statically bound in state.
	} else if(cat == DescriptorCategory::accelStruct) {
		// TODO: do we need to copy acceleration structures?
		// If we ever change this here, also change copyableDescriptorSame
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
				auto& subpass = rp.desc.subpasses[info.hookedSubpass];
				auto& depthStencil = nonNull(subpass.pDepthStencilAttachment);
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
			state->indirectCopy.ensure(dev, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

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
	auto& cmdHook = *dev.commandHook;

	auto& build = accelStructBuilds.emplace_back();
	build.command = &cmd;

	// 2. initialize data
	for(auto i = 0u; i < cmd.buildInfos.size(); ++i) {
		auto& srcBuildInfo = cmd.buildInfos[i];
		auto& rangeInfos = cmd.buildRangeInfos[i];
		auto& accelStruct = *cmd.dsts[i];

		auto& dst = build.builds.emplace_back();
		dst.info = srcBuildInfo;
		// safe to just reference them here, record will stay alive at least
		// until we read it again.
		dst.rangeInfos = cmd.buildRangeInfos[i];
		dst.dst = &accelStruct;
		dst.geoms.resize(dst.info.geometryCount);
		dst.info.pGeometries = dst.geoms.data();

		auto needsInit = (dst.dst->geometryType == VK_GEOMETRY_TYPE_MAX_ENUM_KHR);
		if(dst.dst->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
			// when the accelStruct was built before, but never on the device,
			// we might not have a instance device buffer.
			needsInit = !(std::get<AccelInstances>(dst.dst->data).buffer.buf);
		}

		if(needsInit) {
			dlg_assert(cmd.buildRangeInfos[i].size() == srcBuildInfo.geometryCount);
			initBufs(*dst.dst, srcBuildInfo, cmd.buildRangeInfos[i].data(), true);
		}

		OwnBuffer* dstBuffer {};
		if(dst.dst->geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
			auto& dst = std::get<AccelAABBs>(accelStruct.data);
			dstBuffer = &dst.buffer;
		} else if(dst.dst->geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
			auto& dst = std::get<AccelTriangles>(accelStruct.data);
			dstBuffer = &dst.buffer;
		} else if(dst.dst->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
			auto& dst = std::get<AccelInstances>(accelStruct.data);
			dstBuffer = &dst.buffer;
		} else {
			dlg_error("Invalid VkGeometryTypeKHR: {}", dst.dst->geometryType);
		}

		dlg_assert(dstBuffer);
		dlg_assert(dstBuffer->size);

		VkBufferDeviceAddressInfo addrInfo {};
		addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		addrInfo.buffer = dstBuffer->buf;
		auto dstAddress = dev.dispatch.GetBufferDeviceAddress(dev.handle, &addrInfo);
		dlg_assert(dstAddress);

		auto dstOff = 0u;
		for(auto g = 0u; g < dst.info.geometryCount; ++g) {
			auto& range = rangeInfos[g];
			auto& srcGeom = srcBuildInfo.pGeometries ?
				srcBuildInfo.pGeometries[g] : *srcBuildInfo.ppGeometries[g];
			auto& dstGeom = dst.geoms[g];
			dstGeom = srcGeom;

			if(srcGeom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
				dlg_error("TODO: need shader");
			} else if(srcGeom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
				auto& srcTris = srcGeom.geometry.triangles;
				auto& dstTris = dstGeom.geometry.triangles;

				// copy vertices
				dlg_assert(cmdHook.accelStructVertCopy_);
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

				dstTris.indexData.hostAddress = {};
				dstTris.vertexData.hostAddress = dstBuffer->map + dstOff;
				dstTris.indexType = VK_INDEX_TYPE_NONE_KHR;

				dstOff += range.primitiveCount * sizeof(AccelTriangles::Triangle);
			} else if(srcGeom.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
				// TODO: resolve indirection via custom compute shader
				auto& inis = srcGeom.geometry.instances;
				dlg_assertm(!inis.arrayOfPointers,
					"TODO: arrayOfPointers not supported yet");

				auto srcAddr = inis.data.deviceAddress;
				srcAddr += range.primitiveOffset;
				auto size = sizeof(VkAccelerationStructureInstanceKHR) * range.primitiveCount;
				performCopy(dev, cb, srcAddr, *dstBuffer, dstOff, size);
				dstGeom.geometry.instances.arrayOfPointers = false;
				dstGeom.geometry.instances.data.hostAddress = dstBuffer->map + dstOff;

				dstOff += size;
			} else {
				dlg_fatal("invalid geometry type {}", srcGeom.geometryType);
			}
		}
	}

	// We have to restore the original compute state here since the application
	// won't expect that we changed it for the following commands.
	// TODO: also restore push constants
	bind(dev, cb, cmd.savedComputeState);
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
		// no other reason for this record to be finished except
		// invalidation
		dlg_assert(!hook);
	}
}

// submission
CommandHookSubmission::CommandHookSubmission(CommandHookRecord& rec,
		Submission& subm) : record(&rec) {
	dlg_assert(rec.state || !rec.accelStructBuilds.empty());
	dlg_assert(!rec.writer);
	rec.writer = &subm;
	descriptorSnapshot = rec.record->lastDescriptorState;
}

CommandHookSubmission::~CommandHookSubmission() {
	// it's important we have this here (as opposed to in this->finish)
	// for vkQueueSubmit failure cases.
	if(record) {
		dlg_assert(record->record);
		record->writer = nullptr;
	}
}

void CommandHookSubmission::finish(Submission& subm) {
	ZoneScoped;
	dlg_assert(record->writer == &subm);

	// In this case the hook was removed, no longer interested in results.
	// Since we are the only submission left to the record, it can be
	// destroyed.
	if(!record->hook) {
		record->writer = nullptr;
		dlg_assert(!contains(record->record->hookRecords, record));
		delete record;
		record = nullptr;
		return;
	}

	finishAccelStructBuilds();

	// when the record has not state, we don't have to transmit anything
	if(!record->state) {
		dlg_assert(record->hcommand.empty());
		return;
	}

	transmitTiming();

	auto& state = record->hook->completed.emplace_back();
	state.record = IntrusivePtr<CommandRecord>(record->record);
	state.match = record->match;
	state.state = record->state;
	state.command = record->hcommand;
	state.descriptorSnapshot = std::move(this->descriptorSnapshot);
	state.submissionID = subm.parent->globalSubmitID;

	dlg_assertm(record->hook->completed.size() < 32,
		"Hook state overflow detected");

	// indirect command readback
	if(record->hook->copyIndirectCmd) {
		auto& bcmd = *record->hcommand.back();

		if(auto* cmd = dynamic_cast<const DrawIndirectCountCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size >= 4u);
			auto* count = reinterpret_cast<const u32*>(record->state->indirectCopy.map);
			record->state->indirectCommandCount = *count;
			dlg_assert(record->state->indirectCommandCount <= cmd->maxDrawCount);

			auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assertlm(dlg_level_warn,
				record->state->indirectCopy.size >= 4 + *count * cmdSize,
				"Indirect command readback buffer too small; commands missing");

			// auto cmdsSize = cmdSize * record->state->indirectCommandCount;
			// record->state->indirectCopy.cpuCopy(4u, cmdsSize);
			// record->state->indirectCopy.copyOffset = 0u;
			record->state->indirectCopy.invalidateMap();
		} else if(auto* cmd = dynamic_cast<const DrawIndirectCmd*>(&bcmd)) {
			[[maybe_unused]] auto cmdSize = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			dlg_assert(record->state->indirectCopy.size == cmd->drawCount * cmdSize);

			record->state->indirectCommandCount = cmd->drawCount;
			record->state->indirectCopy.invalidateMap();
		} else if(dynamic_cast<const DispatchIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size == sizeof(VkDispatchIndirectCommand));

			record->state->indirectCommandCount = 1u;
			record->state->indirectCopy.invalidateMap();
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

void CommandHookSubmission::finishAccelStructBuilds() {
	for(auto& buildCmd : record->accelStructBuilds) {
		for(auto& build : buildCmd.builds) {
			auto& dst = nonNull(build.dst);
			dlg_assert(build.rangeInfos.size() == build.info.geometryCount);
			dlg_assert(build.dst->geometryType != VK_GEOMETRY_TYPE_MAX_ENUM_KHR);

			// we only need this additional copy/retrieve step when a top
			// level accelStruct (with instances) was built, otherwise
			// we already copied everything into the right position.
			if(build.dst->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
				copyBuildData(dst, build.info, build.rangeInfos.data(), true);
			}
		}
	}
}

CommandHookState::CommandHookState() {
	++DebugStats::get().aliveHookStates;
}

CommandHookState::~CommandHookState() {
	dlg_assert(DebugStats::get().aliveHookStates > 0);
	--DebugStats::get().aliveHookStates;
}

} // namespace vil
