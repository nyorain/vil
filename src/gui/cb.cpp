#include <gui/cb.hpp>
#include <gui/gui.hpp>
// #include <gui/bufferRead.hpp>
#include <bytes.hpp>
#include <queue.hpp>
#include <cb.hpp>
#include <imguiutil.hpp>
#include <commands.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>

namespace fuen {

// TimeCommandHook
/*
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
	void finish(Device& dev) noexcept override;
	std::unique_ptr<CommandHookSubmissionImpl> submit() override;
	void invalidate() override { hook = nullptr; }
	SimultaneousSubmitHook simulataneousBehavior() const override {
		return SimultaneousSubmitHook::skip;
	}
};

struct TimeCommandHookSubmission : CommandHookSubmissionImpl {
	TimeCommandHookRecord* record {};

	TimeCommandHookSubmission(TimeCommandHookRecord& rec) : record(&rec) {}
	void finish(Device& dev) noexcept override;
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

void TimeCommandHookRecord::finish(Device& dev) noexcept {
	dev.dispatch.DestroyQueryPool(dev.handle, queryPool, nullptr);
	queryPool = {};
}

void TimeCommandHookSubmission::finish(Device& dev) noexcept {
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
*/

// CommandBufferGui
// CommandBufferGui::CommandBufferGui() = default;
// CommandBufferGui::~CommandBufferGui() = default;

void CommandBufferGui::draw() {
	if(!record_) {
		ImGui::Text("No record selected");
		return;
	}

	auto& dev = gui_->dev();

	if(!record_->cb) {
		unsetDestroyedLocked(*record_);
	} else {
		dlg_assert(record_->destroyed.empty());
	}

	// Command list
	ImGui::Columns(2);
	ImGui::BeginChild("Command list", {400, 0});

	if(updateFromGroup_ && record_->group) {
		auto lastRecord = record_->group->lastRecord.get();
		if(lastRecord != record_.get()) {
			record_ = record_->group->lastRecord;
			command_ = CommandDesc::find(record_->commands, desc_);
			// TODO: reset hooks/update desc?
		}
	}

	ImGui::PushID(dlg::format("{}", record_->group).c_str());

	// TODO: add selector ui to filter out various commands/don't
	// show sections etc. Should probably pass a struct DisplayDesc
	// to displayCommands instead of various parameters
	auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
	auto nsel = displayCommands(record_->commands, command_, flags);
	auto commandChanged = false;
	if(!nsel.empty() && nsel.front() != command_) {
		commandChanged = true;
		command_ = nsel.front();
		desc_ = CommandDesc::get(*record_->commands, nsel);

		if(record_->group) {
			record_->group->hook.desc(desc_);
		}
	}

	if(commandChanged) {
		auto* drawIndirect = dynamic_cast<const DrawIndirectCmd*>(command_);
		if(drawIndirect && drawIndirect->indexed) {
			drawIndirect = nullptr;
		}
		if(indirectHook_ && !drawIndirect) {
			record_->group->hook.remove(*indirectHook_);
			indirectHook_ = nullptr;
		} else if(drawIndirect && !indirectHook_) {
			if(!record_->group) {
				// TODO
				// NOTE: we could query the group for a command buffer during
				// its "end" call. Might lead to other problems tho.
				// disabledQueryTime("Record was never submitted");
				dlg_warn("Record was never submitted");
				dlg_assert(!timeHook_);
			} else if(!record_->cb && !updateFromGroup_) {
				// TODO
				// disabledQueryTime("Record isn't valid and viewed statically");
				dlg_warn("Record isn't valid and viewed statically");
				// resetTimeHook = true;
			} else {
				auto impl = std::make_unique<IndirectCommandHook>(IndirectCommandHook::Type::draw);
				indirectHook_ = impl.get();
				record_->group->hook.add(std::move(impl));
			}
		}

		record_->group->hook.desc(desc_);
	}

	ImGui::PopID();

	ImGui::EndChild();
	ImGui::NextColumn();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	if(command_) {
		// Inspector
		command_->displayInspector(*gui_);

		// TODO: restructure!
		auto* drawIndirect = dynamic_cast<const DrawIndirectCmd*>(command_);
		if(drawIndirect) {
			if(indirectHook_ && indirectHook_->count) {
				auto span = ReadBuf(indirectHook_->data);
				auto cmd = read<VkDrawIndirectCommand>(span);
				record_->group->hook.desc(desc_);

				imGuiText("firstVertex: {}", cmd.firstVertex);
				imGuiText("vertexCount: {}", cmd.vertexCount);
				imGuiText("firstInstance: {}", cmd.firstInstance);
				imGuiText("instanceCount: {}", cmd.instanceCount);
			}
		}

		// Show own general gui
		auto resetTimeHook = false;
		auto disabledQueryTime = [&](const auto& tooltip) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);

			ImGui::Checkbox("Query Time", &queryTime_);
			if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::BeginTooltip();
				ImGui::Text("%s", tooltip);
				ImGui::EndTooltip();
			}

			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		};

		if(!record_->group) {
			// NOTE: we could query the group for a command buffer during
			// its "end" call. Might lead to other problems tho.
			disabledQueryTime("Record was never submitted");
			dlg_assert(!timeHook_);
		} else if(!record_->cb && !updateFromGroup_) {
			disabledQueryTime("Record isn't valid and viewed statically");
			resetTimeHook = true;
		} else {
			ImGui::Checkbox("Query Time", &queryTime_);
			if(queryTime_) {
				if(!timeHook_) {
					auto impl = std::make_unique<TimeCommandHook>();
					timeHook_ = impl.get();

					record_->group->hook.add(std::move(impl));
					record_->group->hook.desc(desc_);
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
				resetTimeHook = true;
			}
		}

		if(resetTimeHook) {
			if(timeHook_) {
				record_->group->hook.remove(*timeHook_);
				timeHook_ = nullptr;
			}
		}
	}

	ImGui::EndChild();
	ImGui::Columns();
}

void CommandBufferGui::select(IntrusivePtr<CommandRecord> record,
		bool updateFromGroup) {
	updateFromGroup_ = updateFromGroup;

	// Reset old time hooks
	if(record_) {
		if(timeHook_) {
			record_->group->hook.remove(*timeHook_);
			timeHook_ = nullptr;
		}

		if(indirectHook_) {
			record_->group->hook.remove(*indirectHook_);
			indirectHook_ = nullptr;
		}
	} else {
		dlg_assert(!timeHook_);
		dlg_assert(!indirectHook_);
	}

	command_ = {};

	// NOTE: we could try to find new command matching old description
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
