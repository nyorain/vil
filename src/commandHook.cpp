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

// CommandHook
VkCommandBuffer CommandHook::hook(CommandBuffer& hooked, CommandHookSubmission& subm) {
	subm = {};
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
	}
}

void CommandHook::add(std::unique_ptr<CommandHookImpl> impl) {
	dlg_assert(impl);

	auto order = impl->order();
	auto cmp = [](const auto& impl, int order) { return impl.order() < order; };
	auto it = std::lower_bound(impls_.begin(), impls_.end(), order, cmp);
	impls_.insert(it, std::move(impl));

	invalidateRecordings();
}

void CommandHook::remove(CommandHookImpl& impl) {
	auto order = impl.order();
	auto cmp = [](const auto& impl, int order) { return impl.order() < order; };

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
}

CommandHook::~CommandHook() {
	invalidateRecordings();
}

// CommandHookRecord
void CommandHookRecord::finish() {
	// Finish is only ever called from our hook entry in the CommandRecord.
	// If hook was unset, the TimeCommandHook is being destroyed
	// and removed us manually. Only in that case submissions might be left.
	dlg_assert(!hook || submissionCount == 0);
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
		hook->records_ = next;
	}
}

// CommandHookSubmission
CommandHookSubmission::~CommandHookSubmission() {
	dlg_assert(record && record->record);
	dlg_assert(record->submissionCount >= 0u);

	for(auto& impl : impls) {
		impl->finish(record->record->device());
	}

	--record->submissionCount;

	// In this case the record was invalidated and should be destroyed
	// as soon as possible. If we are the last submission, delete it.
	if(!record->hook && record->submissionCount == 0u) {
		delete record;
	}
}

// NOTE WIP
struct TimeCommandHook : CommandHookImpl {
	u64 lastTime {};
	std::unique_ptr<CommandHookRecordImpl> createRecord(CommandBuffer&, Command&) override;
};

struct TimeCommandHookRecord : CommandHookRecordImpl {
	TimeCommandHook* hook {};
	VkQueryPool queryPool {};

	TimeCommandHookRecord(TimeCommandHook& hook, CommandBuffer& cb);
	void recordBeforeHooked(Device&, VkCommandBuffer, Command&) override;
	void recordAfterHookedChildren(Device&, VkCommandBuffer, Command&) override;
	void finish(Device& dev) override;
	std::unique_ptr<CommandHookSubmissionImpl> submit() override;
	void invalidate() override { hook = nullptr; }
	SimultaneousSubmitHook simulataneousBehavior() const override {
		return SimultaneousSubmitHook::skip;
	}
};

struct TimeCommandHookSubmission : CommandHookSubmissionImpl {
	TimeCommandHookRecord* record {};

	TimeCommandHookSubmission(TimeCommandHookRecord& rec) : record(&rec) {}
	void finish(Device& dev) override;
};

// TimeCommandHook
std::unique_ptr<CommandHookRecordImpl> TimeCommandHook::createRecord(
		CommandBuffer& cb, Command&) {
	return std::make_unique<TimeCommandHookRecord>(*this, cb);
}

// TimeCommandHookRecord
TimeCommandHookRecord::TimeCommandHookRecord(TimeCommandHook& xhook, CommandBuffer& cb) {
	hook = &xhook;

	auto& dev = *cb.dev;

	VkQueryPoolCreateInfo qci {};
	qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qci.queryCount = 2u;
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &this->queryPool));
	nameHandle(dev, this->queryPool, "TimeCommandHookRecord:queryPool");
}

void TimeCommandHookRecord::recordBeforeHooked(Device& dev, VkCommandBuffer cb, Command&) {
	dev.dispatch.CmdResetQueryPool(cb, this->queryPool, 0, 2);

	auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	dev.dispatch.CmdWriteTimestamp(cb, stage0, this->queryPool, 0);
}

void TimeCommandHookRecord::recordAfterHookedChildren(Device& dev, VkCommandBuffer cb, Command&) {
	auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dev.dispatch.CmdWriteTimestamp(cb, stage1, this->queryPool, 1);
}

std::unique_ptr<CommandHookSubmissionImpl> TimeCommandHookRecord::submit() {
	return std::make_unique<TimeCommandHookSubmission>(*this);
}

void TimeCommandHookRecord::finish(Device& dev) {
	dev.dispatch.DestroyQueryPool(dev.handle, queryPool, nullptr);
	queryPool = {};
}

void TimeCommandHookSubmission::finish(Device& dev) {
	// Hook was removed or record invalidated. We cannot return
	// our results back to the hook.
	if(!record->hook) {
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
	record->hook->lastTime = diff;
}

//
struct IndirectCommandHook : CommandHookImpl {
	enum class Type {
		Draw,
		DrawCount,
		Dispatch,
	};

	Type type {};
	u32 count {};
	std::vector<std::byte> data;

	std::unique_ptr<CommandHookRecordImpl> createRecord(CommandBuffer&, Command&) override;
};

struct IndirectCommandHookRecord : CommandHookRecordImpl {
	IndirectCommandHook* hook {};
	VkDeviceMemory dstMemory {};
	VkBuffer dstBuffer {};
	void* map {};
	Command* hooked {}; // TODO(opt) could be stored in CommandHookRecord

	IndirectCommandHookRecord(IndirectCommandHook&, CommandBuffer&, Command&);
	void recordBeforeHooked(Device&, VkCommandBuffer, Command&) override;
	void finish(Device& dev) override;
	std::unique_ptr<CommandHookSubmissionImpl> submit() override;
	void invalidate() override { hook = nullptr; }
	SimultaneousSubmitHook simulataneousBehavior() const override {
		return SimultaneousSubmitHook::skip;
	}
};

struct IndirectCommandHookSubmission : CommandHookSubmissionImpl {
	IndirectCommandHookRecord* record {};

	IndirectCommandHookSubmission(IndirectCommandHookRecord& r) : record(&r) {}
	void finish(Device& dev) override;
};

std::unique_ptr<CommandHookRecordImpl> IndirectCommandHook::createRecord(
		CommandBuffer& cb, Command& hooked) {
	return std::make_unique<IndirectCommandHookRecord>(*this, cb, hooked);
}

IndirectCommandHookRecord::IndirectCommandHookRecord(
		IndirectCommandHook& xhook, CommandBuffer& cb, Command& hooked) {
	this->hook = &xhook;
	this->hooked = &hooked;

	VkDeviceSize dstSize {};
	switch(hook->type) {
		case IndirectCommandHook::Type::Dispatch: {
			// auto& cmd = dynamic_cast<DispatchIndirectCmd&>(hooked);
			dstSize = sizeof(VkDispatchIndirectCommand);
			break;
		} case IndirectCommandHook::Type::Draw: {
			auto& cmd = dynamic_cast<DrawIndirectCmd&>(hooked);
			VkDeviceSize stride = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd.stride ? cmd.stride : stride;
			dstSize = cmd.drawCount * stride;
			break;
		} case IndirectCommandHook::Type::DrawCount: {
			auto& cmd = dynamic_cast<DrawIndirectCountCmd&>(hooked);
			VkDeviceSize stride = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd.stride ? cmd.stride : stride;
			// we get the maximum copy size via maxDrawCount but also
			// via the remaining buffer size.
			auto remSize = cmd.buffer->ci.size - cmd.offset;
			dstSize = std::min(cmd.maxDrawCount * stride, remSize);
			dstSize += sizeof(u32); // for the count
			break;
		}
	}

	auto& dev = *cb.dev;
	VkBufferCreateInfo bci {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bci.size = dstSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bci, nullptr, &dstBuffer));
	nameHandle(dev, this->dstBuffer, "IndirectCommandHookRecord:dstBuffer");

	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, dstBuffer, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &dstMemory));
	nameHandle(dev, this->dstMemory, "IndirectCommandHookRecord:dstMemory");

	VK_CHECK(dev.dispatch.BindBufferMemory(dev.handle, dstBuffer, dstMemory, 0));

	// map memory
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, dstMemory, 0, VK_WHOLE_SIZE, 0, &map));
}

void IndirectCommandHookRecord::recordBeforeHooked(Device& dev,
		VkCommandBuffer cb, Command& hooked) {

	// TODO: technically, we probably need a barrier for out dstBuffer
	// after the copie(s) as well.
	switch(hook->type) {
		case IndirectCommandHook::Type::Dispatch: {
			auto& cmd = dynamic_cast<DispatchIndirectCmd&>(hooked);

			VkBufferCopy copy;
			copy.srcOffset = cmd.offset;
			copy.dstOffset = 0u;
			copy.size = sizeof(VkDispatchIndirectCommand);

			VkBufferMemoryBarrier barrier {};
			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.buffer = cmd.buffer->handle;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			barrier.size = copy.size;
			barrier.offset = copy.srcOffset;

			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			dev.dispatch.CmdCopyBuffer(cb, cmd.buffer->handle, dstBuffer, 1, &copy);

			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			break;
		} case IndirectCommandHook::Type::Draw: {
			auto& cmd = dynamic_cast<DrawIndirectCmd&>(hooked);

			VkDeviceSize stride = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd.stride ? cmd.stride : stride;
			auto dstSize = cmd.drawCount * stride;

			VkBufferCopy copy;
			copy.srcOffset = cmd.offset;
			copy.dstOffset = 0u;
			copy.size = dstSize;

			VkBufferMemoryBarrier barrier {};
			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.buffer = cmd.buffer->handle;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			barrier.size = copy.size;
			barrier.offset = copy.srcOffset;

			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			dev.dispatch.CmdCopyBuffer(cb, cmd.buffer->handle, dstBuffer, 1, &copy);

			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			break;
		} case IndirectCommandHook::Type::DrawCount: {
			// auto& cmd = dynamic_cast<DrawIndirectCountCmd&>(hooked);
			dlg_error("Not implemented");
			break;
		}
	}
}

std::unique_ptr<CommandHookSubmissionImpl> IndirectCommandHookRecord::submit() {
	return std::make_unique<IndirectCommandHookSubmission>(*this);
}

void IndirectCommandHookRecord::finish(Device& dev) {
	dev.dispatch.FreeMemory(dev.handle, dstMemory, nullptr);
	dev.dispatch.DestroyBuffer(dev.handle, dstBuffer, nullptr);
}

//
void IndirectCommandHookSubmission::finish(Device& dev) {
	if(!record->hook) {
		return;
	}

	// TODO: only call on non-coherent memory
	VkMappedMemoryRange range {};
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	range.memory = record->dstMemory;
	VK_CHECK(dev.dispatch.InvalidateMappedMemoryRanges(dev.handle, 1, &range));

	switch(record->hook->type) {
		case IndirectCommandHook::Type::Dispatch: {
			record->hook->count = 1u;
			auto size = sizeof(VkDispatchIndirectCommand);
			record->hook->data.resize(size);
			std::memcpy(record->hook->data.data(), record->map, size);
			break;
		} case IndirectCommandHook::Type::Draw: {
			auto& cmd = dynamic_cast<DrawIndirectCmd&>(*record->hooked);
			VkDeviceSize cmdSize = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto stride = cmd.stride ? cmd.stride : cmdSize;

			record->hook->count = cmd.drawCount;
			record->hook->data.resize(cmd.drawCount * cmdSize);

			for(auto i = 0u; i < cmd.drawCount; ++i) {
				auto src = reinterpret_cast<std::byte*>(record->map) + i * stride;
				auto dst = record->hook->data.data() + i * cmdSize;
				std::memcpy(dst, src, cmdSize);
			}

			break;
		} case IndirectCommandHook::Type::DrawCount: {
			dlg_error("Not implemented");
			break;
		}
	}
}

} // namespace fuen

