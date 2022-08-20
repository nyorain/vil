#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/fontAwesome.hpp>
#include <gui/commandDisplay.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <queue.hpp>
#include <ds.hpp>
#include <swapchain.hpp>
#include <threadContext.hpp>
#include <image.hpp>
#include <rp.hpp>
#include <shader.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <command/match.hpp>
#include <util/bytes.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <util/profiling.hpp>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <bitset>

namespace vil {

using HookTarget = CommandHook::HookTarget;
using HookTargetType = CommandHook::HookTargetType;
using UpdateMode = CommandSelection::UpdateMode;
using SelectionType = CommandSelection::SelectionType;

// CommandBufferGui
void CommandBufferGui::init(Gui& gui) {
	gui_ = &gui;
	commandFlags_ = CommandType(~(CommandType::end | CommandType::bind | CommandType::query));
	commandViewer_.init(gui);
	selector_.init(gui_->dev());
}

CommandBufferGui::~CommandBufferGui() = default;

void CommandBufferGui::draw(Draw& draw) {
	ZoneScoped;

	if(!record_ && selector_.updateMode() != UpdateMode::swapchain) {
		imGuiText("No record selected");
		return;
	}

	auto& dev = gui_->dev();
	auto& hook = *dev.commandHook;

	// Possibly update mode
	auto modeName = [](CommandSelection::UpdateMode mode) {
		switch(mode) {
			case UpdateMode::none: return "Static";
			case UpdateMode::commandBuffer: return "CommandBuffer";
			case UpdateMode::any: return "Any";
			case UpdateMode::swapchain: return "Swapchain";
			default:
				dlg_error("unreachable");
				return "";
		}
	};

	auto updateMode = selector_.updateMode();
	if(updateMode != UpdateMode::swapchain) {
		if(ImGui::BeginCombo("Update Source", modeName(updateMode))) {
			if(ImGui::Selectable("None")) {
				selector_.updateMode(UpdateMode::none);
			}

			if(!selector_.cb() && ImGui::Selectable("Any")) {
				selector_.updateMode(UpdateMode::any);
			}

			// TODO: broken I guess? Needs support for automatically
			// selecting cb in Selector
			// auto cb = IntrusiveWrappedPtr<CommandBuffer>(selector_.cb());
			// if(!cb) {
			// 	std::lock_guard lock(dev.mutex);
			// 	cb = IntrusivePtr<CommandWrappedBuffer>(record_->cb);
			// }
			// if(cb && ImGui::Selectable("CommandBuffer")) {
			// 	selector_.updateMode(UpdateMode::commandBuffer);
			// }

			ImGui::EndCombo();
		}
	} else if(!gui_->dev().swapchain) {
		clearSelection(true);
		return;
	}

	// Settings
	if(ImGui::Button(ICON_FA_WRENCH)) {
		ImGui::OpenPopup("Command Selector");
	}

	if(ImGui::BeginPopup("Command Selector")) {
		auto val = commandFlags_.value();

		imGuiText("Visible commands");
		ImGui::CheckboxFlags("Bind", &val, u32(CommandType::bind));
		ImGui::CheckboxFlags("Draw", &val, u32(CommandType::draw));
		ImGui::CheckboxFlags("Dispatch", &val, u32(CommandType::dispatch));
		ImGui::CheckboxFlags("Transfer", &val, u32(CommandType::transfer));
		ImGui::CheckboxFlags("Sync", &val, u32(CommandType::sync));
		ImGui::CheckboxFlags("End", &val, u32(CommandType::end));
		ImGui::CheckboxFlags("Query", &val, u32(CommandType::query));
		ImGui::CheckboxFlags("Other", &val, u32(CommandType::other));
		commandFlags_ = CommandType(val);

		ImGui::Separator();
		ImGui::Checkbox("Unused Descriptor Bindings", &commandViewer_.showUnusedBindings_);

		// TODO: this needs some love.
		// - Disable in frames where the window was scrolled? and then
		//   store the new scroll offset instead? This currently prevents scrolling
		ImGui::Separator();
		ImGui::Checkbox("Focus Selected", &focusSelected_);

		ImGui::Separator();
		int updateTime = std::chrono::duration_cast<std::chrono::milliseconds>(updateTick_.interval).count();
		ImGui::SliderInt("Update time (ms)", &updateTime, 0, 1000);
		updateTick_.interval = std::chrono::duration_cast<UpdateTicker::Clock::duration>(
			std::chrono::milliseconds(updateTime));

		ImGui::EndPopup();
	}

	ImGui::SameLine();

	// TODO: don't show this checkbox (or set it to true and disable?)
	//   when we are viewing an invalidated record without updating.
	auto updateInHook = ImGui::Checkbox("Freeze State", &selector_.freezeState);
	// NOTE: gui.cpp changes hook.freeze on window hide. So we need to
	//   set it to false again if needed.
	//   But we don't always update it, in case of new-selection,
	//   selector_.freezeState is true but hook.freeze is false.
	//   All this update logic is somewhat messy, think of cleaner logic
	updateInHook |= (hook.freeze.load() && !selector_.freezeState);
	if(updateInHook) {
		hook.freeze.store(selector_.freezeState);
	}

	if(gui_->showHelp && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("This will freeze the state of the command you are viewing,\n"
			"e.g. the image/buffer content and measured time.\n"
			"This does not affect updating of the commands shown on the left.");
	}

	if(selector_.updateMode() != UpdateMode::none) {
		ImGui::SameLine();
		ImGui::Checkbox("Freeze Commands", &freezeCommands_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"This will freeze the commands shown on the left.\n"
				"Useful to avoid flickering or just to inspect the current state.\n"
				"Note that the per-command state you are viewing (e.g. image/buffer\n"
				"content or measured time) will still be updated, use the 'Freeze State' checkbox");
		}
	}

	// == logical state update ==
	auto doUpdate = updateTick_.tick();
	if(doUpdate) {
		auto hookUpdated = selector_.update();
		if(hookUpdated && !freezeCommands_) {
			// TODO PERF: we could re-use the matching data from the selector
			// here in many cases instead of running our own match
			// in updateFromSelector.
			updateFromSelector();
		}

		auto selType = selector_.selectionType();
		if(hookUpdated && selType == CommandSelection::SelectionType::command) {
			updateCommandViewer(false);
		}
	}

	// always clear the completed hooks, no matter if we update
	// the state or not.
	// TODO XXX: don't do it like this. Imagine a case where the application
	// has a low framerate and our window a way higher one.
	// We clear this way too often and don't find anything completed
	// during the update tick
	// But we also don't want to keep like 100 completed states around, ugh.
	// Maybe use something ring-buffer like instead? Directly inside CommandHook?
	hook.clearCompleted();

	// force-update shown batches when it's been too long
	// TODO: do something like that also for non-swapchain mode?
	//   when matching records are submitted but they don't contain the
	//   selected command anymore.
	updateMode = selector_.updateMode();
	if(updateMode == UpdateMode::swapchain && !selector_.freezeState && doUpdate) {
		auto lastPresent = dev.swapchain->frameSubmissions[0].presentID;
		auto statePresent = selector_.hookStateSwapchainPresent();
		if(selector_.submission() && lastPresent > statePresent + 4) {
			auto diff = lastPresent - statePresent;

			ImGui::SameLine();
			imGuiText("Command not found in {} frames", diff);

			// force update
			if(!freezeCommands_) {
				dlg_trace("force command update");
				updateRecords(dev.swapchain->frameSubmissions[0].batches);
			}
		}
	}

	// TODO: re-add someting like this but in more beautiful
	// Maybe to the settings popup?
	// if(mode_ == UpdateMode::none) {
	// 	imGuiText("Showing static record");
	// } else if(mode_ == UpdateMode::commandBuffer) {
	// 	dlg_assert(!record_->cb || record_->cb == cb_);
	// 	dlg_assert(cb_ && cb_->lastRecordPtrLocked());
	// 	imGuiText("Updating from Command Buffer");
	// 	ImGui::SameLine();
	// 	refButton(*gui_, *cb_);
	// } else if(mode_ == UpdateMode::any) {
	// 	imGuiText("Updating from any records");
	// } else if(mode_ == UpdateMode::swapchain) {
	// 	auto& sc = *gui_->dev().swapchain;
	// 	imGuiText("Showing per-present commands from");
	// 	ImGui::SameLine();
	// 	refButton(*gui_, sc);
	// }

	ImGui::Separator();

	// Command list
	auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
	if(!ImGui::BeginTable("RecordViewer", 2, flags, ImGui::GetContentRegionAvail())) {
		return;
	}

	ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, 250.f);
	ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	// ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 20, 20, 255));
	ImGui::BeginChild("Command list", {0, 0});

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 4.f));

	if(updateMode == UpdateMode::swapchain) {
		displayFrameCommands();
	} else {
		displayRecordCommands();
	}

	ImGui::PopStyleVar(2);

	ImGui::EndChild();
	// ImGui::PopStyleColor();
	ImGui::TableNextColumn();

	// command info
	ImGui::BeginChild("Command Info");

	switch(selector_.selectionType()) {
		case SelectionType::none:
			imGuiText("Nothing selected yet");
			break;
		case SelectionType::submission: {
			dlg_assert(updateMode == UpdateMode::swapchain);
			auto* subm = selector_.submission();
			dlg_assert(subm);
			if(!subm) {
				imGuiText("Error!");
				break;
			}

			refButtonExpect(*gui_, subm->queue);
			imGuiText("{} records", subm->submissions.size());
			imGuiText("submissionID: {}", subm->submissionID);
			break;
		} case SelectionType::record: {
			auto* rec = selector_.record().get();
			dlg_assert(rec);
			refButtonD(*gui_, rec->cb);
			imGuiText("cb name: {}", rec->cbName ? rec->cbName : "<unnamed>");
			imGuiText("broken labels: {}{}", std::boolalpha, rec->brokenHierarchyLabels);
			imGuiText("record id: {}", rec->recordID);
			imGuiText("refCount: {}", rec->refCount);
			imGuiText("num hook records: {}", rec->hookRecords.size());
			imGuiText("num secondaries: {}", rec->secondaries.size());
			imGuiText("num pipeLayouts: {}", rec->used.pipeLayouts.size());
			imGuiText("num gfx pipes: {}", rec->used.graphicsPipes.size());
			imGuiText("num compute pipes: {}", rec->used.computePipes.size());
			imGuiText("num rt pipes: {}", rec->used.rtPipes.size());
			imGuiText("builds accel structs: {}", rec->buildsAccelStructs);
			break;
		} case SelectionType::command:
			commandViewer_.draw(draw);
			break;
	}

	ImGui::EndChild();
	ImGui::EndTable();
}

void CommandBufferGui::select(IntrusivePtr<CommandRecord> record, Command* cmd) {
	assertNotOwned(gui_->dev().mutex);

	// Unset hooks
	clearSelection(true);

	record_ = record;
	if(cmd) {
		command_ = findHierarchy(*record_, *cmd);
		dlg_assert(!command_.empty());

		// we want to open all sections up to the selected command
		for(auto* hcmd : command_) {
			dlg_assert(!!hcmd->children() == !!dynamic_cast<const ParentCommand*>(hcmd));
			if(hcmd->children()) {
				openedSections_.insert(static_cast<const ParentCommand*>(hcmd));
			}
		}
	}

	selector_.select(record, command_);
	updateFromSelector();
}

void CommandBufferGui::select(IntrusivePtr<CommandRecord> record, CommandBufferPtr cb) {
	assertNotOwned(gui_->dev().mutex);

	clearSelection(true);
	record_ = std::move(record);
	selector_.select(std::move(cb), record, {});
	updateFromSelector();
}

void CommandBufferGui::showSwapchainSubmissions() {
	auto& dev = gui_->dev();
	assertNotOwned(dev.mutex);
	dlg_assert(dev.swapchain);

	std::vector<FrameSubmission> lastFrame;
	{
		std::lock_guard lock(dev.mutex);
		lastFrame = dev.swapchain->frameSubmissions[0].batches;
	}

	clearSelection(true);
	selector_.select(std::move(lastFrame), u32(-1), nullptr, {});
	updateFromSelector();
}

void CommandBufferGui::destroyed(const Handle&) {
	// TODO: something to do here?
}

void CommandBufferGui::displayFrameCommands() {
	if(frame_.empty() &&
			!gui_->dev().swapchain->frameSubmissions[0].batches.empty()) {
		dlg_warn("how did this happen?");
		frame_ = gui_->dev().swapchain->frameSubmissions[0].batches;
	}

	const Command* selectedCommand = nullptr;
	if(!command_.empty()) {
		selectedCommand = command_.back();
	}

	for(auto b = 0u; b < frame_.size(); ++b) {
		auto& batch = frame_[b];
		if(b > 0) {
			ImGui::Separator();
		}

		const auto id = dlg::format("vkQueueSubmit:{}", b);
		auto flags = int(ImGuiTreeNodeFlags_FramePadding);
		if(batch.submissions.empty()) {
			// empty submissions are displayed differently.
			// There is nothing more disappointing than expanding a
			// tree node just to see that it's empty :(
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
		} else {
			flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
		}

		// check if this vkQueueSubmit was selected
		if(submission_ == &batch && !record_) {
			dlg_assert(command_.empty());
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		const auto opened = openedSubmissions_.count(&batch);
		ImGui::SetNextItemOpen(opened);

		const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "vkQueueSubmit");
		const auto labelStartX = ImGui::GetItemRectMin().x + 30;
		if(ImGui::IsItemClicked() && ImGui::GetMousePos().x > labelStartX) {
			submission_ = &batch;
			record_ = nullptr;
			command_ = {};

			auto submID = u32(submission_ - frame_.data());
			selector_.select(frame_, submID, record_, command_);
			commandViewer_.unselect();
		}

		// check if open state changed
		if(open && !opened) {
			openedSubmissions_.insert(&batch);
		} else if(!open && opened) {
			openedSubmissions_.erase(&batch);
		}

		if(!open) {
			continue;
		}

		if(!batch.submissions.empty()) {
			ImGui::Separator();
		}

		// we don't want as much space as tree nodes
		auto s = 0.3 * ImGui::GetTreeNodeToLabelSpacing();
		ImGui::Unindent(s);

		for(auto r = 0u; r < batch.submissions.size(); ++r) {
			auto& rec = batch.submissions[r];
			if(r > 0) {
				ImGui::Separator();
			}

			// extra tree node for every submission
			const auto id = dlg::format("Commands:{}", r);
			auto name = "<Unnamed>";
			if(rec->cbName) {
				name = rec->cbName;
			}

			auto flags = ImGuiTreeNodeFlags_FramePadding |
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_OpenOnDoubleClick;

			// check if record was selected
			if(record_ == rec.get() && command_.empty()) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			const auto opened = openedRecords_.count(rec.get());
			ImGui::SetNextItemOpen(opened);

			const auto open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", name);
			const auto labelStartX = ImGui::GetItemRectMin().x + 30;
			if(ImGui::IsItemClicked() && ImGui::GetMousePos().x > labelStartX) {
				submission_ = &batch;
				record_ = rec;
				command_ = {};

				auto submID = u32(submission_ - frame_.data());
				selector_.select(frame_, submID, record_, command_);
				commandViewer_.unselect();
			}

			// check if open state changed
			if(open && !opened) {
				openedRecords_.insert(rec.get());
			} else if(!open && opened) {
				openedRecords_.erase(rec.get());
			}

			if(!open) {
				continue;
			}

			// we don't want as much space as tree nodes
			auto s = 0.3 * ImGui::GetTreeNodeToLabelSpacing();
			ImGui::Unindent(s);

			if(!brokenLabelNesting_ && rec->brokenHierarchyLabels) {
				brokenLabelNesting_ = true;
				// showing end commands in this viewing mode since
				// they aren't implicitly shown via the nesting
				// now anymore and stuff like EndRenderPass is
				// kinda important.
				commandFlags_ |= Command::Type::end;
			}

			DisplayVisitor visitor(openedSections_, selectedCommand,
				commandFlags_, brokenLabelNesting_);
			visitor.jumpToSelection_ = focusSelected_;
			visitor.display(*rec->commands, true);
			auto nsel = std::move(visitor.newSelection_);

			auto newSelection = !nsel.empty() && (
				command_.empty() ||
				nsel.back() != command_.back());
			if(newSelection) {
				submission_ = &batch;
				record_ = rec;
				command_ = std::move(nsel);

				auto submID = u32(submission_ - frame_.data());
				selector_.select(frame_, submID, record_, command_);
				updateCommandViewer(true);
			}

			ImGui::Indent(s);
			ImGui::TreePop();
		}

		ImGui::Indent(s);
		ImGui::TreePop();
	}
}

void CommandBufferGui::displayRecordCommands() {
	dlg_assert(record_);

	auto* selected = command_.empty() ? nullptr : command_.back();

	if(!brokenLabelNesting_ && record_->brokenHierarchyLabels) {
		brokenLabelNesting_ = true;
		// showing end commands in this viewing mode since
		// they aren't implicitly shown via the nesting
		// now anymore and stuff like EndRenderPass is
		// kinda important.
		commandFlags_ |= Command::Type::end;
	}

	DisplayVisitor visitor(openedSections_,selected,
		commandFlags_, brokenLabelNesting_);
	visitor.jumpToSelection_ = focusSelected_;
	visitor.display(*record_->commands, false);
	auto nsel = std::move(visitor.newSelection_);

	auto newSelection = !nsel.empty() && (
		command_.empty() ||
		nsel.back() != command_.back());
	if(newSelection) {
		command_ = std::move(nsel);

		// TODO: add cleaner, automatic overload to Selector?
		if(selector_.cb()) {
			selector_.select(
				selector_.cbPtr(),
				record_, command_);
		} else {
			selector_.select(record_, command_);
		}
	}
}

void CommandBufferGui::clearSelection(bool unselectCommandViewer) {
	if(unselectCommandViewer) {
		commandViewer_.unselect();
	}

	selector_.unselect();

	frame_.clear();
	submission_ = {};
	record_.reset();
	command_.clear();

	openedSections_.clear();
	openedSubmissions_.clear();
	openedRecords_.clear();
}

// util
// The recursion here can be assumed safe, applications nesting many thousand
// levels of sections (only possible with debug labels i guess) are weird
// and we don't need to support them at the moment.
void addMatches(
		const std::unordered_set<const ParentCommand*>& oldSet,
		std::unordered_set<const ParentCommand*>& newSet,
		std::unordered_set<const ParentCommand*>& transitioned,
		const CommandSectionMatch& sectionMatch) {

	if(oldSet.count(sectionMatch.a)) {
		newSet.insert(sectionMatch.b);
		transitioned.insert(sectionMatch.a);
	}

	for(auto& child : sectionMatch.children) {
		addMatches(oldSet, newSet, transitioned, child);
	}
}

void CommandBufferGui::updateRecords(const FrameMatch& frameMatch,
		std::vector<FrameSubmission>&& records) {
	ThreadMemScope tms;

	// find matching openedSections_
	// NOTE: with this algorithm we will implicitly close sections
	// that disappear and then re-appear.
	// Not sure how we could keep that information though.
	// TODO PERF: memory allocations, use allocator? or swap, keep memory over frames
	std::unordered_set<const ParentCommand*> newOpenSections;
	std::unordered_set<const FrameSubmission*> newOpenSubmissions;
	std::unordered_set<const CommandRecord*> newOpenRecords;

	std::unordered_set<const ParentCommand*> transitionedSections;

	for(auto batchMatch : frameMatch.matches) {
		if(openedSubmissions_.count(batchMatch.a)) {
			newOpenSubmissions.insert(batchMatch.b);
			dlg_assert(batchMatch.b - records.data() <= i64(records.size()));
		}

		for(auto& recordMatch : batchMatch.matches) {
			if(openedRecords_.count(recordMatch.a)) {
				newOpenRecords.insert(recordMatch.b);
			}

			for(auto& sectionMatch : recordMatch.matches) {
				addMatches(openedSections_, newOpenSections, transitionedSections, sectionMatch);
			}
		}
	}

	// TODO PERF debugging, remove otherwise
	for(auto& open : openedSections_) {
		if(!transitionedSections.count(open)) {
			if(auto* lbl = dynamic_cast<const BeginDebugUtilsLabelCmd*>(open); lbl) {
				dlg_trace("Losing open label {}", lbl->name);
			} else {
				dlg_trace("Losing open cmd {}", (const void*) open);
			}
		}
	}

	// apply
	frame_ = std::move(records);
	openedSubmissions_ = std::move(newOpenSubmissions);
	openedRecords_ = std::move(newOpenRecords);
	openedSections_ = std::move(newOpenSections);
}

void CommandBufferGui::updateRecords(std::vector<FrameSubmission> records) {
	// update records
	ThreadMemScope tms;
	LinAllocScope localMatchMem(matchAlloc_);
	auto frameMatch = match(tms, localMatchMem, frame_, records);
	updateRecords(frameMatch, std::move(records));
}

void CommandBufferGui::updateRecord(IntrusivePtr<CommandRecord> record) {
	dlg_trace("TODO: implement opened section matching for single record mode");
	record_ = std::move(record);
}

void CommandBufferGui::updateFromSelector() {
	if(selector_.updateMode() == UpdateMode::swapchain) {
		auto frameSpan = selector_.frame();
		updateRecords({frameSpan.begin(), frameSpan.end()});

		if(selector_.submission()) {
			auto submID = selector_.submission() - frameSpan.data();
			submission_ = &frame_[submID];
		} else {
			submission_ = nullptr;
		}

		record_ = selector_.record();
	} else {
		updateRecord(selector_.record());
	}

	auto commandSpan = selector_.command();
	command_ = {commandSpan.begin(), commandSpan.end()};
}

void CommandBufferGui::updateCommandViewer(bool resetState) {
	commandViewer_.select(selector_.record(),
		*selector_.command().back(), selector_.descriptorSnapshot(),
		resetState, selector_.completedHookState());
}

} // namespace vil
