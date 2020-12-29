#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <queue.hpp>
#include <imguiutil.hpp>
#include <commands.hpp>
#include <imgui/imgui.h>
#include <vk/enumString.hpp>

namespace fuen {

// Time Hooking
VkCommandBuffer TimeCommandHook::hook(CommandBuffer& hooked,
		FinishPtr<CommandHookSubmission>& data) {
	dlg_assert(hooked.state() == CommandBuffer::State::executable);

	// Check if it already has a valid record associated
	auto* record = hooked.lastRecordLocked();
	auto* hcommand = CommandDesc::find(record->commands, this->desc);
	if(!hcommand) {
		dlg_warn("Can't hook cb, can't find hooked command");
		return hooked.handle();
	}

	if(record->hook) {
		auto* our = dynamic_cast<TimeCommandHookRecord*>(record->hook.get());

		// TODO: optimization. When only counter does not match we
		// could re-use all resources (and the object itself) given
		// that there are no pending submissions for it.
		if(our && our->hook == this && our->hookCounter == counter) {
			data.reset(new TimeCommandHookSubmission(*our));
			return our->cb;
		}
	}

	auto hook = new TimeCommandHookRecord();
	++hook->refCount;
	record->hook.reset(hook);

	hook->next = this->records;
	if(this->records) {
		this->records->prev = hook;
	}
	this->records = hook;

	hook->hook = this;
	hook->hookCounter = counter;
	hook->record = record;

	auto& dev = *hooked.dev;

	VkCommandBufferAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = dev.queueFamilies[record->queueFamily].commandPool;
	allocInfo.commandBufferCount = 1;

	VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &allocInfo, &hook->cb));
	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, hook->cb);
	nameHandle(dev, hook->cb, "TimeCommandHook:cb");

	VkQueryPoolCreateInfo qci {};
	qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	qci.queryCount = 2u;
	qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &hook->queryPool));
	nameHandle(dev, hook->cb, "TimeCommandHook:queryPool");

	// record
	VkCommandBufferBeginInfo cbbi {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	// if we have to expect the original command buffer being submitted multiple
	// times simulataneously, this hooked buffer might be as well.
	cbbi.flags = record->usageFlags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	VK_CHECK(dev.dispatch.BeginCommandBuffer(hook->cb, &cbbi));

	dev.dispatch.CmdResetQueryPool(hook->cb, hook->queryPool, 0, 2);

	hook->hookRecord(dev, record->commands, hcommand);

	VK_CHECK(dev.dispatch.EndCommandBuffer(hook->cb));

	data.reset(new TimeCommandHookSubmission(*hook));
	return hook->cb;
}

void TimeCommandHook::finish() noexcept {
	if(--refCount == 0) {
		delete this;
	}
}

TimeCommandHook::~TimeCommandHook() {
	auto* rec = records;
	while(rec) {
		auto* next = rec->next;

		rec->hook = nullptr;
		if(rec->record->hook.get() == rec) {
			rec->record->hook.reset();
		}
		rec = next;
	}
}

// record
TimeCommandHookRecord::~TimeCommandHookRecord() {
	// destroy resources
	auto& dev = record->device();
	auto commandPool = dev.queueFamilies[record->queueFamily].commandPool;

	dev.dispatch.FreeCommandBuffers(dev.handle, commandPool, 1, &cb);
	dev.dispatch.DestroyQueryPool(dev.handle, queryPool, nullptr);

	// unlink
	if(next) {
		next->prev = prev;
	}
	if(prev) {
		prev->next = next;
	}
	if(hook && this == hook->records) {
		dlg_assert(!prev);
		hook->records = next;
	}
}

void TimeCommandHookRecord::hookRecord(Device& dev, Command* cmd, Command* hcmd) {
	while(cmd) {
		cmd->record(dev, this->cb);

		auto isHooked = cmd == hcmd;
		if(isHooked) {
			auto stage0 = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dev.dispatch.CmdWriteTimestamp(this->cb, stage0, this->queryPool, 0);
		}

		if(auto sectionCmd = dynamic_cast<const SectionCommand*>(cmd); sectionCmd) {
			hookRecord(dev, sectionCmd->children, hcmd);
		}

		if(isHooked) {
			auto stage1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dev.dispatch.CmdWriteTimestamp(this->cb, stage1, this->queryPool, 1);
		}

		cmd = cmd->next;
	}
}

void TimeCommandHookRecord::finish() noexcept {
	if(--refCount == 0) {
		delete this;
	}

	// EDIT: nope, nvm. See TimeCommandHook destructor. Remove below
#if 0
	// The finish call on the HookRecord is only done when the CommandRecord
	// is destroyed. That can only happen when all submissions using it
	// were finished, in that case the refCount must be 1 and this object
	// destroyed. Otherwise we can't rely on this->record to stay valid.
	// TODO: we don't really need the ref count, do we?
	dlg_assert(refCount == 1);
	delete this;
#endif // 0
}

// submission
TimeCommandHookSubmission::~TimeCommandHookSubmission() {
	auto& dev = record->record->device();
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


// CommandBufferGui
// CommandBufferGui::CommandBufferGui() = default;
// CommandBufferGui::~CommandBufferGui() = default;

void CommandBufferGui::draw() {
	if(!record_) {
		ImGui::Text("No record selected");
		return;
	}

	auto& dev = gui_->dev();

	// Command list
	ImGui::Columns(2);
	ImGui::BeginChild("Command list", {400, 0});

	if(updateFromGroup_ && record_->group) {
		auto lastRecord = record_->group->lastRecord.get();
		if(lastRecord != record_.get()) {
			record_ = record_->group->lastRecord;
			command_ = CommandDesc::find(record_->commands, desc_);
		}
	}

	ImGui::PushID(dlg::format("{}", record_->group).c_str());

	// TODO: add selector ui to filter out various commands/don't
	// show sections etc. Should probably pass a struct DisplayDesc
	// to displayCommands instead of various parameters
	auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
	auto* nsel = displayCommands(record_->commands, command_, flags);
	auto commandChanged = false;
	if(nsel && nsel != command_) {
		commandChanged = true;
		command_ = nsel;
		desc_ = CommandDesc::get(*record_->commands, *nsel);
	}

	ImGui::PopID();

	ImGui::EndChild();
	ImGui::NextColumn();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	if(command_) {
		// Inspector
		command_->displayInspector(*gui_);

		// Show own general gui
		// TODO: we could query the group for a command buffer during
		// its "end" call. Might lead to other problems tho.
		if(!record_->group) {
			ImGui::Text("Was never submitted, can't query time");
			dlg_assert(!timeHook_);
		} else {
			ImGui::Checkbox("Query Time", &queryTime_);
			if(queryTime_) {
				if(!timeHook_) {
					timeHook_.reset(new TimeCommandHook());
					timeHook_->desc = desc_;

					++timeHook_->refCount;
					record_->group->hook.reset(timeHook_.get());
				}

				dlg_assert(record_->group->hook.get() == timeHook_.get());
				if(commandChanged) {
					++timeHook_->counter;
					timeHook_->desc = desc_;
				}

				auto displayDiff = timeHook_->lastTime * dev.props.limits.timestampPeriod;
				auto timeNames = {"ns", "mus", "ms", "s"};

				auto it = timeNames.begin();
				while(displayDiff > 1000.f && (it + 1) != timeNames.end()) {
					++it;
					displayDiff /= 1000.f;

				}

				imGuiText("Time: {} {}", displayDiff, *it);
			} else {
				if(record_->group->hook.get() == timeHook_.get()) {
					record_->group->hook.reset();
				}
				timeHook_.reset();
			}
		}
	}

	ImGui::EndChild();
	ImGui::Columns();
}

void CommandBufferGui::select(CommandBufferGroup& group) {
	dlg_assert(group.lastRecord);
	updateFromGroup_ = true;
	select(group.lastRecord);
}

void CommandBufferGui::select(IntrusivePtr<CommandRecord> record) {
	// Reset old time hooks
	if(record_ && record_->group && record_->group->hook.get() == timeHook_.get()) {
		record_->group->hook.reset();
	}
	timeHook_.reset();

	command_ = {};
	// try to find new command matching old description?
	// if(record_ && !desc_.empty()) {
	// 	command_ = CommandDesc::find(record_->commands, desc_);
	// }

	record_ = std::move(record);
	desc_ = {};
}

void CommandBufferGui::destroyed(const Handle& handle) {
	(void) handle;
	// we don't care as we only deal with recordings that have shared
	// ownership, i.e. are kept alive by us.
}

} // namespace fuen
