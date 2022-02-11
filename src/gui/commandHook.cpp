#include <gui/commandHook.hpp>
#include <device.hpp>
#include <layer.hpp>
#include <cow.hpp>
#include <queue.hpp>
#include <ds.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <threadContext.hpp>
#include <pipe.hpp>
#include <swapchain.hpp>
#include <rp.hpp>
#include <accelStruct.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <gui/gui.hpp>
#include <command/desc.hpp>
#include <command/commands.hpp>
#include <vk/enumString.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>
#include <vk/format_utils.h>
#include <accelStructVertices.comp.spv.h>

#include <copyTex.comp.1DArray.spv.h>
#include <copyTex.comp.u1DArray.spv.h>
#include <copyTex.comp.i1DArray.spv.h>
#include <copyTex.comp.2DArray.spv.h>
#include <copyTex.comp.u2DArray.spv.h>
#include <copyTex.comp.i2DArray.spv.h>
#include <copyTex.comp.3D.spv.h>
#include <copyTex.comp.u3D.spv.h>
#include <copyTex.comp.i3D.spv.h>

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

// TODO: move to cow.hpp?
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

// Expects a and to have the same layout.
// If the descriptor at (bindingID, elemID) needs to be copied by CommandHook,
// returns whether its the same in a and b.
bool copyableDescriptorSame(DescriptorStateRef a, DescriptorStateRef b,
		unsigned bindingID, unsigned elemID) {
	if(a.data == b.data) {
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
	initImageCopyPipes(dev);
	if(hasAppExt(dev, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
		initAccelStructCopy(dev);
	}
}

CommandHook::~CommandHook() {
	auto& dev = *dev_;
	waitIdleImpl(dev);
	dlg_assert(dev.pending.empty());
	invalidateRecordings();

	for(auto& pipe : copyImagePipes_) {
		dev.dispatch.DestroyPipeline(dev.handle, pipe, nullptr);
	}

	dev.dispatch.DestroyPipelineLayout(dev.handle, copyImagePipeLayout_, nullptr);
	dev.dispatch.DestroyDescriptorSetLayout(dev.handle, copyImageDsLayout_, nullptr);

	dev.dispatch.DestroyPipeline(dev.handle, accelStructVertCopy_, nullptr);
	dev.dispatch.DestroyPipelineLayout(dev.handle, accelStructPipeLayout_, nullptr);
}

void CommandHook::initImageCopyPipes(Device& dev) {
	// ds layout
	VkDescriptorSetLayoutBinding bindings[2] {};
	bindings[0].binding = 0u;
	bindings[0].descriptorCount = 1u;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[1].binding = 1u;
	bindings[1].descriptorCount = 1u;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].pImmutableSamplers = &dev.nearestSampler;

	VkDescriptorSetLayoutCreateInfo dslci {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 2u;
	dslci.pBindings = bindings;
	VK_CHECK(dev.dispatch.CreateDescriptorSetLayout(dev.handle, &dslci, nullptr,
		&copyImageDsLayout_));
	nameHandle(dev, copyImageDsLayout_, "CommandHook:copyImage");

	// pipe layout
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcrs[0].offset = 0;
	pcrs[0].size = 8;

	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	plci.setLayoutCount = 1u;
	plci.pSetLayouts = &copyImageDsLayout_;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr,
		&copyImagePipeLayout_));
	nameHandle(dev, copyImagePipeLayout_, "CommandHook:copyImage");

	// pipes
	std::vector<VkComputePipelineCreateInfo> cpis;

	ThreadMemScope ms;
	auto mods = ms.alloc<VkShaderModule>(ShaderImageType::count);
	auto specs = ms.alloc<VkSpecializationInfo>(ShaderImageType::count);

	VkSpecializationMapEntry specEntries[3];
	for(auto i = 0u; i < 3; ++i) {
		specEntries[i].constantID = i;
		specEntries[i].offset = i * sizeof(u32);
		specEntries[i].size = sizeof(u32);
	}

	auto addCpi = [&](span<const u32> spv, const std::array<u32, 3>& groupSizes) {
		auto& spec = specs[cpis.size()];
		spec.dataSize = groupSizes.size() * 4;
		spec.pData = reinterpret_cast<const std::byte*>(groupSizes.data());
		spec.mapEntryCount = groupSizes.size();
		spec.pMapEntries = specEntries;

		VkShaderModuleCreateInfo sci {};
		sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		sci.codeSize = spv.size() * 4;
		sci.pCode = spv.data();

		auto& mod = mods[cpis.size()];
		VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &sci, nullptr, &mod));

		auto& cpi = cpis.emplace_back();
		if(cpis.size() > 1u) {
			cpi.basePipelineIndex = 0u;
		}

		cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		cpi.layout = copyImagePipeLayout_;
		cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &spec;
	};

	const std::array<u32, 3> spec1D {64, 1, 1};
	const std::array<u32, 3> specOther {8, 8, 1};

	addCpi(copyTex_comp_u1DArray_spv_data, spec1D);
	addCpi(copyTex_comp_u2DArray_spv_data, specOther);
	addCpi(copyTex_comp_u3D_spv_data, specOther);

	addCpi(copyTex_comp_i1DArray_spv_data, spec1D);
	addCpi(copyTex_comp_i2DArray_spv_data, specOther);
	addCpi(copyTex_comp_i3D_spv_data, specOther);

	addCpi(copyTex_comp_1DArray_spv_data, spec1D);
	addCpi(copyTex_comp_2DArray_spv_data, specOther);
	addCpi(copyTex_comp_3D_spv_data, specOther);

	VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, VK_NULL_HANDLE,
		cpis.size(), cpis.data(), nullptr, copyImagePipes_));

	for(auto pipe : copyImagePipes_) {
		nameHandle(dev, pipe, "CommandHook:copyImage");
	}

	for(auto mod : mods) {
		dev.dispatch.DestroyShaderModule(dev.handle, mod, nullptr);
	}
}

void CommandHook::initAccelStructCopy(Device& dev) {
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcrs[0].offset = 0;
	pcrs[0].size = 48;

	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr,
		&accelStructPipeLayout_));
	nameHandle(dev, accelStructPipeLayout_, "CommandHook:accelStructVertCopy");

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

bool CommandHook::copiedDescriptorChanged(const CommandHookRecord& record) {
	dlg_assert_or(record.dsState.size() == descriptorCopies.size(), return true);

	dlg_assert(!record.hcommand.empty());
	auto* cmd = record.hcommand.back();
	const DescriptorState* dsState = getDsState(*cmd);

	for(auto i = 0u; i < descriptorCopies.size(); ++i) {
		auto [setID, bindingID, elemID, _1, _2] = descriptorCopies[i];

		// We can safely access the ds here since we know that the record
		// is still valid
		auto& currDs = access(dsState->descriptorSets[setID]);

		dlg_assert_or(record.dsState[i], return true);
		auto& oldCow = *record.dsState[i];
		auto [oldDs, lock] = access(oldCow);

		if(!copyableDescriptorSame(currDs, oldDs, bindingID, elemID)) {
			return true;
		}
	}

	return false;
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

	if(!validTarget || !record_ || hierachy_.empty() || !record.commands->children_) {
		hookNeededForCmd = false;
	}

	// When there is no gui viewing the submissions at the moment, we don't
	// need/want to hook the submission.
	if(!forceHook && (
			!dev.gui || !dev.gui->visible || freeze ||
			dev.gui->activeTab() != Gui::Tab::commandBuffer)) {
		hookNeededForCmd = false;
	}

	// Even when we aren't interested in any command in the record, we have
	// to hook it when it builds acceleration structures.
	auto needBuildHook = record.buildsAccelStructs && hookAccelStructBuilds;
	if(!hookNeededForCmd && !needBuildHook) {
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

	// We have to capture the currently bound descriptors when we really
	// hook the submission.
	// Needed in case the descriptorSet is changed/destroyd later on, also
	// for updateAfterBind
	auto captureDescriptors = [&](const Command& cmd) -> CommandDescriptorSnapshot {
		// when we only hook for e.g. accel struct building there is no
		// need to add cows to the descriptors.
		if(!hookNeededForCmd) {
			return {};
		}

		return snapshotRelevantDescriptorsLocked(cmd);
	};

	if(foundHookRecord) {
		if(hookNeededForCmd) {
			dlg_check({
				// Before calling find, we need to unset the invalidated handles from the
				// commands in hierachy_, find relies on all of them being valid.
				replaceInvalidatedLocked(nonNull(record_));

				auto findRes = find(record.commands->children_, hierachy_, dsState_);
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
		if(hookNeededForCmd && copiedDescriptorChanged(*foundHookRecord)) {
			invalidate(*foundHookRecord);
			foundHookRecord = nullptr;
		}

		if(foundHookRecord) {
			data.reset(new CommandHookSubmission(*foundHookRecord, subm,
				captureDescriptors(*foundHookRecord->hcommand.back())));
			return foundHookRecord->cb;
		}
	}

	FindResult findRes {};
	if(hookNeededForCmd) {
		// Before calling find, we need to unset the invalidated handles from the
		// commands in hierachy_, find relies on all of them being valid.
		replaceInvalidatedLocked(nonNull(record_));

		findRes = find(record.commands->children_, hierachy_, dsState_);
		if(findRes.hierachy.empty()) {
			// Can't find the command we are looking for in this record
			return hooked.handle();
		}

		dlg_assert(findRes.hierachy.size() == hierachy_.size());
	}

	// dlg_trace("hooking command submission; frame {}", dev.swapchain->presentCounter);
	dlg_assertlm(dlg_level_warn, record.hookRecords.size() < 8,
		"Alarmingly high number of hooks for a single record");

	auto descriptors = CommandDescriptorSnapshot {};
	if(!findRes.hierachy.empty()) {
		descriptors = captureDescriptors(*findRes.hierachy.back());
	}

	auto hook = new CommandHookRecord(*this, record,
		std::move(findRes.hierachy), descriptors);
	hook->match = findRes.match;
	record.hookRecords.push_back(FinishPtr<CommandHookRecord>(hook));

	data.reset(new CommandHookSubmission(*hook, subm, std::move(descriptors)));

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

void CommandHook::invalidateRecordings(bool forceAll) {
	// We have to increase the counter to invalidate all past recordings
	++counter_;

	// Destroy all past recordings as soon as possible
	// (they might be kept alive by pending submissions)
	auto* rec = records_;
	while(rec) {
		// important to store this before we potentially destroy rec.
		auto* next = rec->next;

		// we might not want to invalidate recordings that didn't hook a command
		// and were only done for accelStruct builddata copying
		if(forceAll || !rec->hcommand.empty()) {
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
	this->attachmentCopies.clear();
	this->descriptorCopies.clear();
	this->copyTransferSrc = false;
	this->copyTransferDst = false;
	this->transferIdx = 0u;
	invalidateRecordings();
	invalidateData();
}

// record
CommandHookRecord::CommandHookRecord(CommandHook& xhook,
	CommandRecord& xrecord, std::vector<const Command*> hooked,
	const CommandDescriptorSnapshot& descriptors) :
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
			dlg_info("Queue family {} does not support timing queries", xrecord.queueFamily);
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
	info.descriptors = &descriptors;
	initState(info);

	this->dsState.resize(hook->descriptorCopies.size());

	// record
	// we can never submit the cb simulataneously anyways, see CommandHook
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(dev.dispatch.BeginCommandBuffer(this->cb, &cbbi));

	// initial cmd stuff
	if(this->queryPool) {
		dev.dispatch.CmdResetQueryPool(cb, queryPool, 0, 2);
	}

	unsigned maxHookLevel {};
	info.maxHookLevel = &maxHookLevel;

	ZoneScopedN("HookRecord");
	this->hookRecord(record->commands->children_, info);

	VK_CHECK(dev.dispatch.EndCommandBuffer(this->cb));

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
	auto& hook = *dev.commandHook;

	state.reset(new CommandHookState());
	state->copiedAttachments.resize(hook.attachmentCopies.size());
	state->copiedDescriptors.resize(hook.descriptorCopies.size());

	// Find out if final hooked command is inside render pass
	auto preEnd = hcommand.end() - 1;
	for(auto it = hcommand.begin(); it != preEnd; ++it) {
		auto* cmd = *it;
		info.beginRenderPassCmd = dynamic_cast<const BeginRenderPassCmd*>(cmd);
		if(info.beginRenderPassCmd) {
			break;
		}

		// TODO: support for BeginRendering
	}

	// some operations (index/vertex/attachment) copies only make sense
	// inside a render pass.
	dlg_assert(info.beginRenderPassCmd ||
		(!hook.copyVertexBuffers &&
		 !hook.copyIndexBuffers &&
		 hook.attachmentCopies.empty()));

	// when the hooked command is inside a render pass and we need to perform
	// operations (e.g. copies) not possible while inside a render pass,
	// we have to split the render pass around the selected command.
	info.splitRenderPass = info.beginRenderPassCmd &&
		(hook.copyVertexBuffers ||
		 hook.copyIndexBuffers ||
		 !hook.attachmentCopies.empty() ||
		 !hook.descriptorCopies.empty() ||
		 hook.copyIndirectCmd ||
		 (hook.copyTransferDst && dynamic_cast<const ClearAttachmentCmd*>(hcommand.back())));

	if(info.splitRenderPass) {
		auto& rp = *info.beginRenderPassCmd->rp;

		// TODO: we could likely just directly support this (with exception
		// of transform feedback maybe)
		if(hasChain(rp.desc, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
			dlg_warn("Splitting multiview renderpass not implemented");
			info.splitRenderPass = false;
		}
	}

	if(info.splitRenderPass) {
		auto& desc = info.beginRenderPassCmd->rp->desc;

		// TODO PERF: expensive. Iteratives over all commands.
		info.hookedSubpass = info.beginRenderPassCmd->subpassOfDescendant(*hcommand.back());
		dlg_assert(info.hookedSubpass != u32(-1));
		dlg_assert(info.hookedSubpass < desc.subpasses.size());

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
			info.splitRenderPass = false;
			dlg_warn("Can't split render pass (due to resolve attachments)");
		} else {
			auto [rpi0, rpi1, rpi2] = splitInterruptable(desc);
			rp0 = create(dev, rpi0);
			rp1 = create(dev, rpi1);
			rp2 = create(dev, rpi2);
		}
	}
}

void CommandHookRecord::dispatchRecord(Command& cmd, RecordInfo& info) {
	auto& dev = *record->dev;

	if(info.rebindComputeState && cmd.type() == CommandType::dispatch) {
		auto& dcmd = static_cast<const DispatchCmdBase&>(cmd);

		// pipe, descriptors
		bind(dev, this->cb, dcmd.state);

		// push constants
		if(!dcmd.pushConstants.data.empty()) {
			auto data = dcmd.pushConstants.data;
			auto& layout = dcmd.state.pipe->layout;
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

	cmd.record(*record->dev, this->cb);
}

void CommandHookRecord::hookRecordBeforeDst(Command& dst, RecordInfo& info) {
	auto& dev = *record->dev;

	dlg_assert(&dst == hcommand.back());

	if(info.splitRenderPass) {
		dlg_assert(info.beginRenderPassCmd);

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
	} else if(!info.splitRenderPass && !info.beginRenderPassCmd) {
		beforeDstOutsideRp(dst, info);
	}
}

void CommandHookRecord::hookRecordAfterDst(Command& dst, RecordInfo& info) {
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

void CommandHookRecord::hookRecordDst(Command& cmd, RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel cblbl(dev, cb, "vil:hookRecordDst");

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
		if(!info.splitRenderPass) {
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
		if(cmd->type() == CommandType::buildAccelStruct && CommandHook::hookAccelStructBuilds) {
			auto* basCmd = dynamic_cast<BuildAccelStructsCmd*>(cmd);
			auto* basCmdIndirect = dynamic_cast<BuildAccelStructsCmd*>(cmd);
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
				dlg_assert(info.beginRenderPassCmd == beginRpCmd);
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
		const CommandHook::DescriptorCopy& copyDesc,
		CommandHookState::CopiedDescriptor& dst,
		IntrusivePtr<DescriptorSetCow>& dstCow) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:copyDs");

	const DescriptorState* dsState = getDsState(bcmd);
	if(!dsState) {
		dlg_error("Copying descriptor binding failed: Unsupported descriptor command");
		return;
	}

	auto [setID, bindingID, elemID, _1, imageAsBuffer] = copyDesc;

	// NOTE: we have to check for correct sizes here since the
	// actual command might have changed (for an updated record)
	// and the selected one not valid anymore.
	if(setID >= dsState->descriptorSets.size()) {
		dlg_error("setID out of range");
		return;
	}

	auto it = info.descriptors->states.find(dsState->descriptorSets[setID].dsEntry);
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

				if(imageAsBuffer) {
					// we don't ever use that buffer in a submission again
					// so we can ignore queue families
					auto& dstBuf = dst.data.emplace<OwnBuffer>();
					initAndSampleCopy(dev, cb, dstBuf, *imgView->img, layout,
						subres, {}, imageViews, descriptorSets);
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
			auto dynOffs = dsState->descriptorSets[setID].dynamicOffsets;
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
		initAndCopy(dev, cb, dstBuf, 0u, nonNull(elem.buffer),
			off, size, {});
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
	} else if(cat == DescriptorCategory::accelStruct) {
		// TODO: do we need to copy acceleration structures?
		// If we ever change this here, also change copyableDescriptorSame
		//
		// What we need to do here is this (-> AccelStructState rework):
		// Copy a ref ptr to the current state of the used AccelStruct.
		// That state should take into account all commands previously
		// executed in this submission or from other submissions to the
		// same queue. But it also needs to consider other queues:
		// All submissions that have already finished on other queues
		// or are chained to this submission. While already here we could
		// make sure that there aren't any non-finished submissions to other
		// queues that are building the accelStruct and are not chained
		// to this submission. That would be a sync hazard and undefined anyways.
	} else {
		dlg_error("Unimplemented");
	}
}

void CommandHookRecord::copyAttachment(const Command& bcmd,
		AttachmentType type, unsigned attID,
		CommandHookState::CopiedAttachment& dst) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:copyAttachment");

	// NOTE: written in a general way. We might be in a RenderPass
	// or a {Begin, End}Rendering section (i.e. dynamicRendering).
	const RenderPassInstanceState* rpi {};

	if(auto* dcmd = dynamic_cast<const DrawCmdBase*>(&bcmd); dcmd) {
		rpi = dcmd->state.rpi;
	} else if(auto* ccmd = dynamic_cast<const ClearAttachmentCmd*>(&bcmd); ccmd) {
		rpi = ccmd->rpi;
	} else {
		dlg_error("Invalid command fdor copy attachment");
		return;
	}

	dlg_assert_or(rpi, return);

	span<const ImageView* const> attachments;

	switch(type) {
		case AttachmentType::color:
			// written like this since old GCC versions seem to have problems with
			// our span conversion constructor.
			// see https://github.com/nyorain/vil/runs/5014322209?check_suite_focus=true
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
	auto layout = VK_IMAGE_LAYOUT_GENERAL; // layout between rp splits, see rp.cpp

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

void CommandHookRecord::copyTransfer(Command& bcmd, RecordInfo& info) {
	dlg_assert(hook->copyTransferDst != hook->copyTransferSrc);
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

	auto idx = hook->transferIdx;
	if(hook->copyTransferSrc) {
		std::optional<CopyImage> img;
		std::optional<CopyBuffer> buf;

		if(auto* cmd = dynamic_cast<const CopyImageCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->copies[idx].srcSubresource)};
		} else if(auto* cmd = dynamic_cast<const BlitImageCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->blits[idx].srcSubresource)};
		} else if(auto* cmd = dynamic_cast<const CopyImageToBufferCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->copies[idx].imageSubresource)};
		} else if(auto* cmd = dynamic_cast<const ResolveImageCmd*>(&bcmd); cmd) {
			img = {cmd->src, cmd->srcLayout, toRange(cmd->regions[idx].srcSubresource)};
		} else if(auto* cmd = dynamic_cast<const CopyBufferCmd*>(&bcmd); cmd) {
			auto [offset, size] = minMaxInterval({{cmd->regions[idx]}}, true);
			buf = {cmd->src, offset, size};
		} else if(auto* cmd = dynamic_cast<const CopyBufferToImageCmd*>(&bcmd); cmd) {
			auto texelSize = FormatTexelSize(cmd->dst->ci.format);
			auto [offset, size] = minMaxInterval({{cmd->copies[idx]}}, texelSize);
			buf = {cmd->src, offset, size};
		}

		dlg_assert(img || buf);
		if(img) {
			auto [src, layout, subres] = *img;
			initAndCopy(dev, cb, state->transferImgCopy, *src,
				layout, subres, record->queueFamily);
		} else if(buf) {
			auto [src, offset, size] = *buf;
			if(copyFullTransferBuffer) {
				offset = 0u;
				size = src->ci.size;
			}

			// we don't ever read the buffer from the gfxQueue so we can
			// ignore queueFams here
			initAndCopy(dev, cb, state->transferBufCopy, 0u, *src, offset, size, {});
		}
	} else if(hook->copyTransferDst) {
		std::optional<CopyImage> img;
		std::optional<CopyBuffer> buf;

		if(auto* cmd = dynamic_cast<const CopyImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->copies[idx].dstSubresource)};
		} else if(auto* cmd = dynamic_cast<const BlitImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->blits[idx].dstSubresource)};
		} else if(auto* cmd = dynamic_cast<const CopyBufferToImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->copies[idx].imageSubresource)};
		} else if(auto* cmd = dynamic_cast<const ResolveImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, toRange(cmd->regions[idx].dstSubresource)};
		} else if(auto* cmd = dynamic_cast<const ClearColorImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, cmd->ranges[idx]};
		} else if(auto* cmd = dynamic_cast<const ClearDepthStencilImageCmd*>(&bcmd); cmd) {
			img = {cmd->dst, cmd->dstLayout, cmd->ranges[idx]};
		} else if(auto* cmd = dynamic_cast<const ClearAttachmentCmd*>(&bcmd)) {
			auto& rp = nonNull(info.beginRenderPassCmd->rp);
			auto& fb = nonNull(info.beginRenderPassCmd->fb);

			// TODO: support showing multiple cleared attachments in gui,
			//   allowing to select here which one is copied.
			auto& clearAtt = cmd->attachments[idx];
			u32 attID = clearAtt.colorAttachment;
			if(clearAtt.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
				auto& subpass = rp.desc.subpasses[info.hookedSubpass];
				auto& depthStencil = nonNull(subpass.pDepthStencilAttachment);
				attID = depthStencil.attachment;
			}

			dlg_assert(fb.attachments.size() > attID);
			auto& imgView = nonNull(fb.attachments[attID]);
			auto& src = nonNull(imgView.img);

			// image must be in general layout because we are just between
			// the split render passes
			img = {&src, VK_IMAGE_LAYOUT_GENERAL, imgView.ci.subresourceRange};
		} else if(auto* cmd = dynamic_cast<const CopyBufferCmd*>(&bcmd); cmd) {
			auto [offset, size] = minMaxInterval({{cmd->regions[idx]}}, false);
			buf = {cmd->dst, offset, size};
		} else if(auto* cmd = dynamic_cast<const CopyImageToBufferCmd*>(&bcmd); cmd) {
			auto texelSize = FormatTexelSize(cmd->src->ci.format);
			auto [offset, size] = minMaxInterval({{cmd->copies[idx]}}, texelSize);
			buf = {cmd->dst, offset, size};
		} else if(auto* cmd = dynamic_cast<const FillBufferCmd*>(&bcmd); cmd) {
			buf = {cmd->dst, cmd->offset, cmd->size};
		} else if(auto* cmd = dynamic_cast<const UpdateBufferCmd*>(&bcmd); cmd) {
			buf = {cmd->dst, cmd->offset, cmd->data.size()};
		}

		dlg_assert(img || buf);
		if(img) {
			auto [src, layout, subres] = *img;
			initAndCopy(dev, cb, state->transferImgCopy, *src,
				layout, subres, record->queueFamily);
		} else if(buf) {
			auto [src, offset, size] = *buf;
			if(copyFullTransferBuffer) {
				offset = 0u;
				size = src->ci.size;
			}

			// we don't ever read the buffer from the gfxQueue so we can
			// ignore queueFams here
			initAndCopy(dev, cb, state->transferBufCopy, 0u, *src, offset, size, {});
		}
	}
}

void CommandHookRecord::beforeDstOutsideRp(Command& bcmd, RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:beforeDstOutsideRp");

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
		DebugLabel lbl(dev, cb, "vil:copyInderectCmd");

		// we don't ever read the buffer from the gfxQueue so we can
		// ignore queueFams here
		if(auto* cmd = dynamic_cast<DrawIndirectCmd*>(&bcmd)) {
			VkDeviceSize stride = cmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd->stride ? cmd->stride : stride;
			auto dstSize = cmd->drawCount * stride;
			initAndCopy(dev, cb, state->indirectCopy,  0u,
				nonNull(cmd->buffer), cmd->offset, dstSize, {});
		} else if(auto* cmd = dynamic_cast<DispatchIndirectCmd*>(&bcmd)) {
			auto size = sizeof(VkDispatchIndirectCommand);
			initAndCopy(dev, cb, state->indirectCopy, 0u,
				nonNull(cmd->buffer), cmd->offset, size, {});
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
		} else if(auto* cmd = dynamic_cast<TraceRaysIndirectCmd*>(&bcmd)) {
			auto size = sizeof(VkTraceRaysIndirectCommandKHR);
			initAndCopy(dev, cb, state->indirectCopy, cmd->indirectDeviceAddress,
				size, {});
		} else {
			dlg_error("Unsupported indirect command");
		}
	}

	// attachments
	for(auto [i, ac] : enumerate(hook->attachmentCopies)) {
		if(ac.before) {
			copyAttachment(bcmd, ac.type, ac.id, state->copiedAttachments[i]);
		}
	}

	// descriptor state
	for(auto [i, dc] : enumerate(hook->descriptorCopies)) {
		if(dc.before) {
			copyDs(bcmd, info, dc, state->copiedDescriptors[i], dsState[i]);
		}
	}

	auto* drawCmd = dynamic_cast<DrawCmdBase*>(&bcmd);

	// We might use the vertex/index buffer copies when rendering the ui
	// later on so we have to care about queue families
	auto queueFams = combineQueueFamilies({{record->queueFamily, dev.gfxQueue->family}});

	// PERF: we could support tighter buffer bounds for indirect/indexed draw
	// calls. See node 1749 for a sketch using a couple of compute shaders,
	// basically emulating an indirect transfer.
	// PERF: for non-indexed/non-indirect draw calls we know the exact
	// sizes of vertex/index buffers to copy, we could use that.
	auto maxVertIndSize = maxBufCopySize;

	if(hook->copyVertexBuffers) {
		DebugLabel lbl(dev, cb, "vil:copyVertexBuffers");

		dlg_assert(drawCmd);
		for(auto& vertbuf : drawCmd->state.vertices) {
			auto& dst = state->vertexBufCopies.emplace_back();
			if(!vertbuf.buffer) {
				continue;
			}

			auto size = std::min(maxVertIndSize, vertbuf.buffer->ci.size - vertbuf.offset);
			initAndCopy(dev, cb, dst, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
				nonNull(vertbuf.buffer), vertbuf.offset, size, queueFams);
		}
	}

	if(hook->copyIndexBuffers) {
		DebugLabel lbl(dev, cb, "vil:copyIndexBuffers");

		dlg_assert(drawCmd);
		auto& inds = drawCmd->state.indices;
		if(inds.buffer) {
			auto size = std::min(maxVertIndSize, inds.buffer->ci.size - inds.offset);
			initAndCopy(dev, cb, state->indexBufCopy, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				nonNull(inds.buffer), inds.offset, size, queueFams);
		}
	}

	// transfer
	if(hook->copyTransferBefore && (hook->copyTransferDst || hook->copyTransferSrc)) {
		copyTransfer(bcmd, info);
	}
}

void CommandHookRecord::afterDstOutsideRp(Command& bcmd, RecordInfo& info) {
	auto& dev = *record->dev;
	DebugLabel lbl(dev, cb, "vil:afterDsOutsideRp");

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

	// attachments
	for(auto [i, ac] : enumerate(hook->attachmentCopies)) {
		if(!ac.before) {
			copyAttachment(bcmd, ac.type, ac.id, state->copiedAttachments[i]);
		}
	}

	// descriptor state
	for(auto [i, dc] : enumerate(hook->descriptorCopies)) {
		if(!dc.before) {
			copyDs(bcmd, info, dc, state->copiedDescriptors[i], dsState[i]);
		}
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
	DebugLabel lbl(dev, cb, "vil:beforeBuildAccelStructs");

	auto& cmdHook = *dev.commandHook;

	auto& build = accelStructBuilds.emplace_back();
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

		// TODO: we don't always need this. Not sure how to correctly handle it,
		// we *sometimes* have to resize the buffer to make sure it can fit
		// the new data. Maybe just always call initBufs and handle the logic
		// in there? rename it to ensureBufSizes?
		needsInit = true;
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

// submission
CommandHookSubmission::CommandHookSubmission(CommandHookRecord& rec,
	Submission& subm, CommandDescriptorSnapshot descriptors) :
		record(&rec), descriptorSnapshot(std::move(descriptors)) {
	dlg_assert(rec.state || !rec.accelStructBuilds.empty());
	dlg_assert(!rec.writer);
	rec.writer = &subm;
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

		// unset for our destructor
		record = nullptr;
		return;
	}

	finishAccelStructBuilds();

	// when the record has no state, we don't have to transmit anything
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

	// This usually is a sign of a problem somewhere inside the layer.
	// Either we are not correctly clearing completed states from the gui
	// but still producing new ones or we have just *waaay* to many
	// candidates and should somehow improve matching for this case.
	dlg_assertlm(dlg_level_warn, record->hook->completed.size() < 64,
		"High number of hook states detected");

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
		} else if(dynamic_cast<const TraceRaysIndirectCmd*>(&bcmd)) {
			dlg_assert(record->state->indirectCopy.size == sizeof(VkTraceRaysIndirectCommandKHR));

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
	if(!record->hook->queryTime) {
		return;
	}

	if(!record->queryPool) {
		// The query pool couldn't be created.
		// This could be the case when the queue does not support
		// timing queries. Signal it.
		record->state->neededTime = u64(-1);
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
