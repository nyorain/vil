#include <fwd.hpp>
#include <commandDesc.hpp>
#include <cb.hpp>
#include <util/dlg.hpp>
#include <vk/vulkan.h>
#include <memory>
#include <vector>

namespace vil {

struct CommandHookSubmissionData {
	std::unique_ptr<CommandHookSubmissionData> next;

	virtual ~CommandHookSubmissionData();
};

struct CommandHookRecordData {
	std::unique_ptr<CommandHookRecordData> next;

	virtual std::unique_ptr<CommandHookSubmissionData> submit() { return nullptr; }
	virtual void finish(Device&) {}
	virtual ~CommandHookRecordData() = default;
};

struct CommandHookImpl {
	std::unique_ptr<CommandHookImpl> next {};

	virtual ~CommandHookImpl() = default;
	virtual std::unique_ptr<CommandHookRecordData> createRecord(Device&, CommandBuffer&) { return nullptr; }

	virtual void beforeHooked(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}
	virtual void afterHooked(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}

	virtual void beforeHookedChildren(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}
	virtual void afterHookedChildren(Device&, VkCommandBuffer, CommandHookRecordData*, Command&) {}

	virtual void record(Device&, VkCommandBuffer, CommandHookRecordData*,
		Command& toBeRecorded, Command& hooked) {}

	virtual bool allowSubmitSkip() const { return true; }
	virtual bool allowMultiSubmit() const { return true; }
};

///
struct CommandHookSubmission {
	std::unique_ptr<CommandHookSubmissionData> impls;
	CommandHookRecord* record;
};

struct CommandHookRecord {
	CommandHook* hook {};
	CommandRecord* record {};
	u32 hookCounter {};

	// NOTE: could make this linked list of submissions instead
	u32 submissionCounter {};

	// linked list of records
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

	VkCommandBuffer cb {};

	std::unique_ptr<CommandHookRecordData> impls;
};

struct CommandHook {
	std::unique_ptr<CommandHookImpl> impls;

	u32 counter {};
	std::vector<CommandDesc> desc {};
	CommandHookRecord* records {}; // linked list

	~CommandHook();
	VkCommandBuffer hook(CommandBuffer& hooked);
};

VkCommandBuffer CommandHook::hook(CommandBuffer& hooked) {
	// Check if it already has a valid record associated
	auto* record = hooked.lastRecordLocked();
	auto* hcommand = CommandDesc::find(record->commands, this->desc);
	if(!hcommand) {
		dlg_warn("Can't hook cb, can't find hooked command");
		return hooked.handle();
	}
}

//

struct TimeCommandHook : CommandHookImpl {
	u64 lastTime {};

	std::unique_ptr<CommandHookRecordData> createRecord(Device&, CommandBuffer&) override;
	void record(Device&, VkCommandBuffer cb, CommandHookRecordData* data,
		Command& cmd, Command& hooked) override;
};

struct TimeCommandHookRecord : CommandHookRecordData {
	TimeCommandHook* hook {};
	VkQueryPool queryPool {};
};

struct TimeCommandHookSubmission : CommandHookSubmissionData {
	TimeCommandHookRecord* record {};
	virtual ~TimeCommandHookSubmission();
};

std::unique_ptr<CommandHookRecordData> TimeCommandHook::createRecord(Device& dev,
		CommandBuffer&) {

	auto hook = std::make_unique<TimeCommandHookRecord>();
	hook->hook = this;

	VkQueryPoolCreateInfo qci {};
	qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qci.queryCount = 2u;
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &hook->queryPool));
	nameHandle(dev, hook->queryPool, "TimeCommandHook:queryPool");

	dev.dispatch.CmdResetQueryPool(hook->cb, hook->queryPool, 0, 2);

	return hook;
}

void TimeCommandHook::record(Device& dev, VkCommandBuffer cb,
		CommandHookRecordData* recDataBase, Command& cmd, Command& hooked) {
	auto* recData = (TimeCommandHookRecord*) recDataBase;

	if(&cmd == hooked.next) {
		auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dev.dispatch.CmdWriteTimestamp(this->cb, stage1, this->queryPool, 1);
	}

	if(next) {
		next->record(dev, cb, cmd, hooked);
	} else {
		cmd.record(dev, cb);
	}

	if(&cmd == &hooked) {
		auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dev.dispatch.CmdWriteTimestamp(cb, stage0, recData->queryPool, 0);
	}
}

//

struct IndirectBufferReadHook : CommandHookImpl {
	VkDrawIndirectCommand lastCmd {};
};

struct IndirectBufferReadHookRecord : CommandHookRecordData {
	IndirectBufferReadHook* hook {};
	VkDeviceMemory dstMemory {};
	VkBuffer dstBuffer {};
};

struct IndirectBufferReadHookSubmission : CommandHookSubmissionData {
	IndirectBufferReadHookRecord* record {};
	virtual ~IndirectBufferReadHookSubmission();
};

} // namespace vil

----

size_t totalNumBindings(const DescriptorSetLayout& layout, u32 variableDescriptorCount) {
	if(layout.bindings.empty()) {
		return 0;
	}

	auto& last = layout.bindings.back();
	size_t ret = last.offset;

	if(last.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
		ret += variableDescriptorCount;
	} else {
		ret += last.descriptorCount;
	}

	return ret;
}

---

class ThreadMemoryResource : public std::pmr::memory_resource {
	LinAllocScope* memScope_ {};

	void* do_allocate(std::size_t bytes, std::size_t alignment) override {
		return memScope_->allocBytes(bytes, alignment);
	}

	void do_deallocate(void*, std::size_t, std::size_t) override {
		// no-op
	}

	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
		auto* tmr = dynamic_cast<const ThreadMemoryResource*>(&other);
		if(!tmr) {
			return false;
		}

		return tmr->memScope_ == this->memScope_;
	}
};


