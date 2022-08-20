#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <commandHook/submission.hpp>
#include <commandHook/copy.hpp>
#include <device.hpp>
#include <submit.hpp>
#include <wrap.hpp>
#include <layer.hpp>
#include <cb.hpp>
#include <ds.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <queue.hpp>
#include <threadContext.hpp>
#include <swapchain.hpp>
#include <command/match.hpp>
#include <command/commands.hpp>
#include <vk/enumString.hpp>
#include <util/util.hpp>
#include <util/profiling.hpp>
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

#include <copyTex.comp.1DArray.noformat.spv.h>
#include <copyTex.comp.u1DArray.noformat.spv.h>
#include <copyTex.comp.i1DArray.noformat.spv.h>
#include <copyTex.comp.2DArray.noformat.spv.h>
#include <copyTex.comp.u2DArray.noformat.spv.h>
#include <copyTex.comp.i2DArray.noformat.spv.h>
#include <copyTex.comp.3D.noformat.spv.h>
#include <copyTex.comp.u3D.noformat.spv.h>
#include <copyTex.comp.i3D.noformat.spv.h>

// TODO: instead of doing memory barrier per-resource when copying to
//   our readback buffers, we should probably do just do general memory
//   barriers.
// TODO: We don't really need hookCounter, counter_ anymore (except
//   for asserts). Remove? might be useful in future for threading stuff tho.
// TODO: move hooked command recording out of critical section, if possible.
//   We have the guarantee that all handles in cb stay valid during submission
//   and don't really access global state.
//   Only difficulty: what if hook ops/target is changed in the meantime?
//   Just discard the recording and abort hooking?

namespace vil {

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

	// will invalidate everything, safely
	HookUpdate update;
	update.invalidate = true;
	update.newOps = HookOps {};
	update.newTarget = HookTarget {};
	update.stillNeeded = nullptr;
	this->updateHook(std::move(update));

	for(auto& pipe : sampleImagePipes_) {
		dev.dispatch.DestroyPipeline(dev.handle, pipe, nullptr);
	}

	dev.dispatch.DestroyPipelineLayout(dev.handle, sampleImagePipeLayout_, nullptr);
	dev.dispatch.DestroyDescriptorSetLayout(dev.handle, sampleImageDsLayout_, nullptr);

	dev.dispatch.DestroyPipeline(dev.handle, accelStructVertCopy_, nullptr);
	dev.dispatch.DestroyPipelineLayout(dev.handle, accelStructPipeLayout_, nullptr);
}

void CommandHook::initImageCopyPipes(Device& dev) {
	// ds layout
	VkDescriptorSetLayoutBinding bindings[2] {};
	bindings[0].binding = 0u;
	bindings[0].descriptorCount = 1u;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
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
		&sampleImageDsLayout_));
	nameHandle(dev, sampleImageDsLayout_, "CommandHook:copyImage");

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
	plci.pSetLayouts = &sampleImageDsLayout_;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr,
		&sampleImagePipeLayout_));
	nameHandle(dev, sampleImagePipeLayout_, "CommandHook:sampleCopyImage");

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
		cpi.layout = sampleImagePipeLayout_;
		cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &spec;
	};

	const std::array<u32, 3> spec1D {64, 1, 1};
	const std::array<u32, 3> specOther {8, 8, 1};

	if(dev.shaderStorageImageWriteWithoutFormat) {
		addCpi(copyTex_comp_u1DArray_spv_data, spec1D);
		addCpi(copyTex_comp_u2DArray_spv_data, specOther);
		addCpi(copyTex_comp_u3D_spv_data, specOther);

		addCpi(copyTex_comp_i1DArray_spv_data, spec1D);
		addCpi(copyTex_comp_i2DArray_spv_data, specOther);
		addCpi(copyTex_comp_i3D_spv_data, specOther);

		addCpi(copyTex_comp_1DArray_spv_data, spec1D);
		addCpi(copyTex_comp_2DArray_spv_data, specOther);
		addCpi(copyTex_comp_3D_spv_data, specOther);
	} else {
		addCpi(copyTex_comp_u1DArray_noformat_spv_data, spec1D);
		addCpi(copyTex_comp_u2DArray_noformat_spv_data, specOther);
		addCpi(copyTex_comp_u3D_noformat_spv_data, specOther);

		addCpi(copyTex_comp_i1DArray_noformat_spv_data, spec1D);
		addCpi(copyTex_comp_i2DArray_noformat_spv_data, specOther);
		addCpi(copyTex_comp_i3D_noformat_spv_data, specOther);

		addCpi(copyTex_comp_1DArray_noformat_spv_data, spec1D);
		addCpi(copyTex_comp_2DArray_noformat_spv_data, specOther);
		addCpi(copyTex_comp_3D_noformat_spv_data, specOther);
	}

	VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, VK_NULL_HANDLE,
		cpis.size(), cpis.data(), nullptr, sampleImagePipes_));

	for(auto pipe : sampleImagePipes_) {
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

// TODO: temporary removal of record.dsState due to sync issues.
// Needs to be fixed!
bool CommandHook::copiedDescriptorChanged(const CommandHookRecord& record) {
	// dlg_assert_or(record.dsState.size() == ops_.descriptorCopies.size(), return true);

	dlg_assert(!record.hcommand.empty());
	if(ops_.descriptorCopies.empty()) {
		return false;
	}

	auto* cmd = record.hcommand.back();
	dlg_assert(cmd);
	dlg_assert_or(cmd->type() == CommandType::draw ||
		cmd->type() == CommandType::dispatch ||
		cmd->type() == CommandType::traceRays,
		return false);
	const DescriptorState& dsState =
		static_cast<const StateCmdBase*>(cmd)->boundDescriptors();

	for(auto i = 0u; i < ops_.descriptorCopies.size(); ++i) {
		auto [setID, bindingID, elemID, _1, _2] = ops_.descriptorCopies[i];

		// We can safely access the ds here since we know that the record
		// is still valid
		auto& currDs = access(dsState.descriptorSets[setID]);

		for(auto& binding : currDs.layout->bindings) {
			if(binding.flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT ||
					binding.flags & VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) {
				return true;
			}
		}

	/*
		dlg_assert_or(record.dsState[i], return true);
		auto& oldCow = *record.dsState[i];
		auto [oldDs, lock] = access(oldCow);

		if(!copyableDescriptorSame(currDs, oldDs, bindingID, elemID)) {
			return true;
		}
	*/
	}

	return false;
}

void CommandHook::hook(QueueSubmitter& subm) {
	auto& dev = *dev_;

	// we put all of this in a critical section to protect against changes
	// of target_ and ops_ and the list of hooked records.
	// TODO: might be possible to just use internal mutex, try it.
	// Would help performance, the hooked recording is in the critical
	// section here as well and quite expensive.
	std::lock_guard lock(dev.mutex);

	if(target_.type == HookTargetType::none || freeze.load()) {
		return;
	}

	// TODO: full-submission/record queries!
	if(target_.command.empty()) {
		return;
	}

	const CommandRecord* frameDstRecord {};
	span<const CommandSectionMatch> frameRecMatchData {};
	if(target_.type == HookTargetType::inFrame) {
		if(!dev.swapchain) {
			dlg_warn("no swapchain anymore?!");
			return;
		}

		// get the current frame
		std::vector<FrameSubmission> currFrame =
			dev.swapchain->nextFrameSubmissions.batches;

		// add the new submissions
		auto& curr = currFrame.emplace_back();
		curr.queue = subm.queue;
		curr.submissionID = subm.globalSubmitID;
		for(auto& sub : subm.dstBatch->submissions) {
			for(auto& cb : sub.cbs) {
				dlg_assertm(!cb.hook, "Hooking already hooked submission?!");
				auto rec = cb.cb->lastRecordPtrLocked();
				dlg_assert(rec);
				curr.submissions.push_back(std::move(rec));
			}
		}

		// Match current frame against hook target frame.
		// Since we don't have the full current frame here yet and want
		// to avoid false negatives, we trim the target frame up unto
		// the target submission.
		auto trimmedTargetFrame = span<FrameSubmission>(target_.frame);
		auto off = target_.submissionID;
		dlg_assert(off < target_.frame.size());
		trimmedTargetFrame = trimmedTargetFrame.first(off + 1);

		ThreadMemScope tms;
		LinAllocScope localMatchMem(matchAlloc_);
		auto frameMatch = match(tms, localMatchMem, target_.frame, currFrame);

		for(auto& submMatch : frameMatch.matches) {
			if(submMatch.a != &target_.frame[target_.submissionID]) {
				continue;
			}

			for(auto& recMatch : submMatch.matches) {
				if(recMatch.a == target_.record) {
					frameDstRecord = recMatch.b;
					frameRecMatchData = recMatch.matches;
					break;
				}

			}

			break;
		}
	}

	// iterate through all submitted records and hook them if needed
	for(auto [subID, sub] : enumerate(subm.dstBatch->submissions)) {
		auto& srcSub = subm.submitInfos[subID];

		span<VkCommandBufferSubmitInfo> patchedCbInfos {};

		for(auto [cbID, cb] : enumerate(sub.cbs)) {
			auto& rec = *cb.cb->lastRecordLocked();

			VkCommandBuffer hooked = VK_NULL_HANDLE;
			std::unique_ptr<CommandHookSubmission> hookData;

			if(!freeze.load()) {
				auto hookViaFind = false;
				if(&rec == frameDstRecord) {
					dlg_assert(target_.type == HookTargetType::inFrame);
					hooked = hook(rec, frameRecMatchData, sub, hookData);
				} else if(target_.type == HookTargetType::commandRecord) {
					if(&rec == target_.record) {
						hookViaFind = true;
					}
				} else if(target_.type == HookTargetType::commandBuffer) {
					dlg_assert(rec.cb);
					if(rec.cb == target_.cb.get()) {
						hookViaFind = true;
					}
				} else if(target_.type == HookTargetType::all) {
					hookViaFind = true;
				}

				if(hookViaFind) {
					auto findRes = find(*rec.commands, target_.command,
						target_.descriptors);
					if(findRes.match > 0.f) {
						hooked = doHook(rec, findRes.hierarchy,
							findRes.match, sub, hookData);
					}
				}

				if(!hookData && rec.buildsAccelStructs && hookAccelStructBuilds) {
					dlg_assert(!hooked);
					hooked = doHook(rec, {}, 0.f, sub, hookData);
				}
			}

			dlg_assert(!!hooked == !!hookData);
			if(hookData) {
				if(patchedCbInfos.empty()) {
					// NOTE: important to use subm.memScope instead of
					// some local ThreadMemScope here!
					patchedCbInfos = subm.memScope.copy(srcSub.pCommandBufferInfos,
						srcSub.commandBufferInfoCount);
				}

				patchedCbInfos[cbID].commandBuffer = hooked;
				cb.hook = std::move(hookData);

				subm.lastLayerSubmission = &sub;
			}
		}

		if(!patchedCbInfos.empty()) {
			dlg_assert(patchedCbInfos.size() == srcSub.commandBufferInfoCount);
			srcSub.pCommandBufferInfos = patchedCbInfos.data();
		}
	}
}

VkCommandBuffer CommandHook::hook(CommandRecord& record,
		span<const CommandSectionMatch> matchData,
		Submission& subm,
		std::unique_ptr<CommandHookSubmission>& data) {

	auto hierarchy = span<const Command*>(target_.command);
	span<const CommandSectionMatch> sectionMatches = matchData;
	std::vector<const ParentCommand*> dstHierarchy;

	float dstMatch {1.f};
	auto found = true;
	while(hierarchy.size() > 1 && found) {
		auto foundSection = false;
		for(auto& cmdMatch : sectionMatches) {
			if(cmdMatch.a != hierarchy[0]) {
				continue;
			}

			dstMatch *= eval(cmdMatch.match);
			foundSection = true;
			hierarchy = hierarchy.subspan(1u);
			sectionMatches = cmdMatch.children;
			dstHierarchy.push_back(cmdMatch.b);
			break;
		}

		if(!foundSection) {
			found = false;
		}
	}

	// no hook needed
	if(!found) {
		return VK_NULL_HANDLE;
	}

	auto findResult = find(*dstHierarchy.back(), span(target_.command).last(2),
		target_.descriptors);

	// no hook needed
	if(findResult.hierarchy.empty()) {
		return VK_NULL_HANDLE;
	}

	dstMatch *= findResult.match;

	dlg_assert(findResult.hierarchy.size() == 2u);
	dlg_assert(findResult.hierarchy[0] == dstHierarchy.back());

	ThreadMemScope tms;
	auto cmds = tms.alloc<const Command*>(dstHierarchy.size() + 1);
	std::copy(dstHierarchy.begin(), dstHierarchy.end(), cmds.data());
	cmds[dstHierarchy.size()] = findResult.hierarchy[1];

	return doHook(record, cmds, dstMatch, subm, data);
}

VkCommandBuffer CommandHook::doHook(CommandRecord& record,
		span<const Command*> dstCommand, float dstCommandMatch,
		Submission& subm, std::unique_ptr<CommandHookSubmission>& data) {

	// Check if there already is a valid CommandHookRecord we can use.
	CommandHookRecord* foundHookRecord {};
	CommandHookRecord* foundCompleted = nullptr;
	auto foundCompletedIt = completed_.end();
	auto completedCount = 0u;
	auto hookNeededForCmd = bool(!dstCommand.empty());

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

		// we can't reuse this hook record, it's state is still needded
		if(hookRecord->state.get() == stillNeeded_) {
			continue;
		}

		// the record has completed, its state in our completed list
		auto completedIt = find_if(this->completed_, [&](const CompletedHook& completed) {
			return completed.state == hookRecord->state;
		});
		if(completedIt != completed_.end()) {
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
		dlg_assert(foundCompletedIt != completed_.end());
		dlg_assert(foundCompleted);
		this->completed_.erase(foundCompletedIt);
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

		// we know for sure that all descriptorSets of the to-be-hooked
		// command must still be valid.
		return snapshotRelevantDescriptorsValidLocked(cmd);
	};

	if(foundHookRecord) {
		if(hookNeededForCmd) {
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
	}

	auto descriptors = CommandDescriptorSnapshot {};
	if(!dstCommand.empty()) {
		descriptors = captureDescriptors(*dstCommand.back());
	}

	if(!foundHookRecord) {
		dlg_assertlm(dlg_level_warn, record.hookRecords.size() < 8,
			"Alarmingly high number of hooks for a single record");

		auto hook = new CommandHookRecord(*this, record,
			{dstCommand.begin(), dstCommand.end()},
			descriptors);
		hook->match = dstCommandMatch;
		record.hookRecords.push_back(FinishPtr<CommandHookRecord>(hook));

		foundHookRecord = hook;
	}

	dlg_check({
		dlg_assert(target_.record);

		auto findRes = find(*record.commands, target_.command, target_.descriptors);
		dlg_assert(findRes.match > 0.f);
		dlg_assert(std::equal(
			foundHookRecord->hcommand.begin(), foundHookRecord->hcommand.end(),
			findRes.hierarchy.begin(), findRes.hierarchy.end()));
	});

	data.reset(new CommandHookSubmission(*foundHookRecord, subm, std::move(descriptors)));
	return foundHookRecord->cb;
}

std::vector<CommandHook::CompletedHook> CommandHook::moveCompleted() {
	std::vector<CompletedHook> moved;
	{
		std::lock_guard lock(dev_->mutex);
		moved = std::move(this->completed_);
	}
	return moved;
}

void CommandHook::clearCompleted() {
	(void) moveCompleted();
}

void CommandHook::invalidateRecordings(bool forceAll) {
	std::lock_guard lock(dev_->mutex);

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

CommandHookState::CommandHookState() {
	++DebugStats::get().aliveHookStates;
}

CommandHookState::~CommandHookState() {
	dlg_assert(DebugStats::get().aliveHookStates > 0);
	--DebugStats::get().aliveHookStates;
}

void CommandHook::updateHook(HookUpdate&& update) {
	{
		// make sure we don't destroy these while lock is held
		HookTarget oldTarget;

		{
			std::lock_guard lock(dev_->mutex);

			if(update.newTarget) {
				oldTarget = std::move(target_);
				target_ = std::move(*update.newTarget);

				// validate
				if(target_.type == HookTargetType::inFrame) {
					dlg_assert(target_.submissionID != u32(-1));
					dlg_assert(target_.submissionID < target_.frame.size());
				}
			}

			if(update.newOps) {
				dlg_assert(update.invalidate);
				ops_ = std::move(*update.newOps);
			}

			if(update.stillNeeded) {
				stillNeeded_ = *update.stillNeeded;
			}
		}
	}

	if(update.invalidate) {
		clearCompleted();
		invalidateRecordings();
	}
}

CommandHook::HookOps CommandHook::ops() const {
	std::lock_guard lock(dev_->mutex);
	return ops_;
}

CommandHook::HookTarget CommandHook::target() const {
	std::lock_guard lock(dev_->mutex);
	return target_;
}

} // namespace vil
