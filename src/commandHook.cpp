#include <commandHook.hpp>
#include <commandDesc.hpp>
#include <commands.hpp>
#include <util.hpp>
#include <cb.hpp>
#include <dlg/dlg.hpp>

#include <buffer.hpp> // TODO: remove

namespace fuen {

// CommandHookRecordTerminator
struct CommandHookRecordTerminator : CommandHookRecordImpl {
	static CommandHookRecordTerminator instance;

	virtual void record(Device& dev, VkCommandBuffer cb,
			Command& toBeRecorded, Command&) {
		dlg_assert(!next);
		toBeRecorded.record(dev, cb);
	}
};

CommandHookRecordTerminator CommandHookRecordTerminator::instance {};

// CommandHook
VkCommandBuffer CommandHook::hook(CommandBuffer& hooked, CommandHookSubmission& subm) {
	// subm = {};
	if(this->impls_.empty()) {
		return hooked.handle();
	}

	// Check if it already has a valid record associated
	auto* record = hooked.lastRecordLocked();
	auto* hcommand = CommandDesc::find(record->commands, this->desc_);
	if(!hcommand) {
		dlg_trace("Can't hook cb: can't find hooked command");
		return hooked.handle();
	}

	// TODO, optimization: When only counter does not match we
	// could re-use all resources (and the object itself) given
	// that there are no pending submissions for it.
	if(record->hook && record->hook->hook == this && record->hook->hookCounter == this->counter_) {
		auto simb = record->hook->simulataneousBehavior;
		if(simb == SimultaneousSubmitHook::allow || record->hook->submissionCount == 0) {
			fillSubmission(*record->hook, subm);
			return record->hook->cb;
		}

		if(simb == SimultaneousSubmitHook::skip) {
			// In this case, some impl didn't allow simulataneous submission
			// of the cb but all of them are okay with skipping the submission
			// if there is already a pending submission.
			return hooked.handle();
		}

		dlg_assert(simb == SimultaneousSubmitHook::recreate);
	}

	// check implementations that want/need hook
	std::vector<std::unique_ptr<CommandHookRecordImpl>> recordImpls;
	SimultaneousSubmitHook simb = SimultaneousSubmitHook::allow;
	for(auto& impl : impls_) {
		auto record = impl->createRecord(hooked, *hcommand);
		if(record) {
			if(!recordImpls.empty()) {
				recordImpls.back()->next = record.get();
			}

			auto rsimb = record->simulataneousBehavior();
			if(u32(rsimb) > u32(simb)) {
				simb = rsimb;
			}

			recordImpls.push_back(std::move(record));
		}
	}

	if(recordImpls.empty()) {
		// no hook needed at all
		return hooked.handle();
	}

	recordImpls.back()->next = &CommandHookRecordTerminator::instance;

	auto recordHook = new CommandHookRecord();
	record->hook.reset(recordHook);

	recordHook->impls = std::move(recordImpls);
	recordHook->hook = this;
	recordHook->hookCounter = this->counter_;
	recordHook->simulataneousBehavior = simb;
	recordHook->record = record;

	recordHook->next = this->records_;
	if(this->records_) {
		this->records_->prev = recordHook;
	}
	this->records_ = recordHook;

	// Created hooked cb
	auto& dev = *hooked.dev;

	VkCommandBufferAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = dev.queueFamilies[record->queueFamily].commandPool;
	allocInfo.commandBufferCount = 1;

	VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &allocInfo, &recordHook->cb));
	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, recordHook->cb);
	nameHandle(dev, recordHook->cb, "CommandHookRecord:cb");

	// record
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if(recordHook->simulataneousBehavior == SimultaneousSubmitHook::allow) {
		cbbi.flags = record->usageFlags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	}
	// TODO: adapt other flags?

	VK_CHECK(dev.dispatch.BeginCommandBuffer(recordHook->cb, &cbbi));
	this->recordHook(*recordHook, record->commands, *hcommand);
	VK_CHECK(dev.dispatch.EndCommandBuffer(recordHook->cb));

	fillSubmission(*recordHook, subm);
	return recordHook->cb;
}

void CommandHook::fillSubmission(CommandHookRecord& record, CommandHookSubmission& subm) {
	++record.submissionCount;
	subm.record = &record;

	for(auto& impl : record.impls) {
		auto data = impl->submit();
		if(data) {
			subm.impls.push_back(std::move(data));
		}
	}
}

void CommandHook::recordHook(CommandHookRecord& record, Command* cmd, Command& hooked) {
	dlg_assert(!record.impls.empty());
	auto& dev = record.record->device();

	while(cmd) {
		if(cmd == &hooked) {
			for(auto& impl : record.impls) {
				impl->recordBeforeHooked(dev, record.cb, *cmd);
			}
		}

		record.impls.front()->record(dev, record.cb, *cmd, hooked);

		if(auto parentCmd = dynamic_cast<const ParentCommand*>(cmd); parentCmd) {
			recordHook(record, parentCmd->children(), hooked);
		}

		if(cmd == &hooked) {
			for(auto& impl : reversed(record.impls)) {
				impl->recordAfterHookedChildren(dev, record.cb, *cmd);
			}
		}

		cmd = cmd->next;
	}
}

void CommandHook::add(std::unique_ptr<CommandHookImpl> impl) {
	dlg_assert(impl);

	auto order = impl->order();
	auto cmp = [](const auto& impl, int order) { return impl->order() < order; };
	auto it = std::lower_bound(impls_.begin(), impls_.end(), order, cmp);
	impls_.insert(it, std::move(impl));

	invalidateRecordings();
}

void CommandHook::remove(CommandHookImpl& impl) {
	auto order = impl.order();
	auto cmp = [](const auto& impl, int order) { return impl->order() < order; };

	auto it = std::lower_bound(impls_.begin(), impls_.end(), order, cmp);
	dlg_assert(it != impls_.end() && it->get() == &impl);
	impls_.erase(it);

	invalidateRecordings();
}

void CommandHook::desc(std::vector<CommandDesc> desc) {
	desc_ = std::move(desc);
	invalidateRecordings();
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

		// inform all impls that they were invalidated.
		for(auto& impl : rec->impls) {
			impl->invalidate();
		}
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

CommandHook::~CommandHook() {
	invalidateRecordings();
}

// CommandHookRecord
void CommandHookRecord::finish() noexcept {
	if(submissionCount == 0) {
		delete this;
	}
}

CommandHookRecord::~CommandHookRecord() {
	// We can be sure that record is still alive here since when the
	// record is destroyed, all its submissions must have finished as well.
	// And then we would have been destroyed this via the finish() command (see
	// the assertions there)
	dlg_assert(record);
	dlg_assert(submissionCount == 0);

	auto& dev = record->device();
	for(auto& impl : impls) {
		impl->finish(dev);
	}

	// destroy resources
	auto commandPool = dev.queueFamilies[record->queueFamily].commandPool;
	dev.dispatch.FreeCommandBuffers(dev.handle, commandPool, 1, &cb);

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

// CommandHookSubmission
CommandHookSubmission::~CommandHookSubmission() {
	// Unused CommandHookSubmission
	if(!record) {
		return;
	}

	dlg_assert(record->record);
	dlg_assert(record->submissionCount > 0u);

	for(auto& impl : impls) {
		impl->finish(record->record->device());
	}

	// Call all destructors before we potentially destroy the record.
	impls.clear();

	--record->submissionCount;

	// In this case the record was invalidated and should be destroyed
	// as soon as possible. If we are the last submission, delete it.
	if(!record->hook && record->submissionCount == 0u) {
		delete record;
	}
}

} // namespace fuen

