#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif

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
#include <serialize/serialize.hpp>
#include <serialize/util.hpp>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <command/match.hpp>
#include <nytl/bytes.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <util/profiling.hpp>
#include <vkutil/enumString.hpp>
#include <imgio/file.hpp>
#include <vk/format_utils.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <bitset>
#include <filesystem>

namespace fs = std::filesystem;

namespace vil {

using HookTarget = CommandHook::Target;
using HookTargetType = CommandHook::TargetType;
using UpdateMode = CommandSelection::UpdateMode;
using SelectionType = CommandSelection::SelectionType;

// CommandBufferGui
void CommandRecordGui::init(Gui& gui) {
	gui_ = &gui;
	commandFlags_ = CommandCategory(~(CommandCategory::end | CommandCategory::bind | CommandCategory::query));
	commandViewer_.init(gui);
	selector_.init(gui_->dev());
}

CommandRecordGui::~CommandRecordGui() = default;

void CommandRecordGui::draw(Draw& draw) {
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
			case UpdateMode::localCapture: return "LocalCapture";
			default:
				dlg_error("unreachable");
				return "";
		}
	};

	auto updateMode = selector_.updateMode();
	IntrusivePtr<Swapchain> swapchain;
	if(updateMode == UpdateMode::swapchain) {
		swapchain = gui_->dev().swapchain();
		if(!swapchain) {
			clearSelection(true);
			return;
		}
	} else if(updateMode != UpdateMode::localCapture) {
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
	}

	// Settings
	if(ImGui::Button(ICON_FA_WRENCH)) {
		ImGui::OpenPopup("Command Selector");
	}

	if(ImGui::BeginPopup("Command Selector")) {
		auto val = commandFlags_.value();

		imGuiText("Visible commands");
		ImGui::CheckboxFlags("Bind", &val, u32(CommandCategory::bind));
		ImGui::CheckboxFlags("Draw", &val, u32(CommandCategory::draw));
		ImGui::CheckboxFlags("Dispatch", &val, u32(CommandCategory::dispatch));
		ImGui::CheckboxFlags("Transfer", &val, u32(CommandCategory::transfer));
		ImGui::CheckboxFlags("Sync", &val, u32(CommandCategory::sync));
		ImGui::CheckboxFlags("End", &val, u32(CommandCategory::end));
		ImGui::CheckboxFlags("Query", &val, u32(CommandCategory::query));
		ImGui::CheckboxFlags("Other", &val, u32(CommandCategory::other));
		commandFlags_ = CommandCategory(val);

		ImGui::Separator();
		ImGui::Checkbox("Unused Descriptor Bindings", &commandViewer_.showUnusedBindings_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Show descriptor bindings that are not used "
				"by a command. Their contents and resources can still be shown");
		}

		ImGui::Checkbox("Show single-sections", &showSingleSections_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Show implicit sections, e.g. the single subpass "
				"of a 1-subpass renderpass or the single execute of a CmdExecuteCommands");
		}

		// TODO: this needs some love.
		// - Disable in frames where the window was scrolled? and then
		//   store the new scroll offset instead? This currently prevents scrolling
		//  - make this work for parent commands (needs changes in DisplayVisitor I guess)
		// ImGui::Separator();
		// ImGui::Checkbox("Focus Selected", &focusSelected_);

		// TODO: WIP
		ImGui::Checkbox("Freeze Commands on Sparse Bind", &freezeOnSparseBind_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Will freeze the commands as soon as a vkQueueBindSparse "
				"command is encountered to allow its inspection");
		}

		ImGui::Separator();
		int updateTime = std::chrono::duration_cast<std::chrono::milliseconds>(updateTick_.interval).count();
		ImGui::SliderInt("Update time (ms)", &updateTime, 0, 1000);
		updateTick_.interval = std::chrono::duration_cast<UpdateTicker::Clock::duration>(
			std::chrono::milliseconds(updateTime));

		ImGui::EndPopup();
	}

	ImGui::SameLine();

	// TODO: don't show this (or set it to true and disable?)
	//   when we are viewing an invalidated record without updating.
	// auto updateInHook = toggleButton(ICON_FA_PAUSE, selector_.freezeState);
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
		// toggleButton(ICON_FA_LIST, freezeCommands_);
		ImGui::Checkbox("Freeze Commands", &freezeCommands_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"This will freeze the commands shown on the left.\n"
				"Useful to avoid flickering or just to inspect the current state.\n"
				"Note that the per-command state you are viewing (e.g. image/buffer\n"
				"content or measured time) will still be updated, use the 'Freeze State' checkbox");
		}
	}

	// toggle button
	if(selector_.selectionType() == SelectionType::none) {
		actionFullscreen_ = false;
	} else {
		// TODO: maybe align this to the right? might be more intuitive
		ImGui::SameLine();

		// ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);
		// ImGui::SameLine();

		toggleButton(ICON_FA_ARROWS_ALT, actionFullscreen_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Maximize the selected command viewer.\n"
				"This will hide the command and I/O view");
		}

		if(selector_.selectionType() == SelectionType::command) {
			if(commandViewer_.showBeforeCheckbox_) {
				ImGui::SameLine();
				if(toggleButton(ICON_FA_UNDO, commandViewer_.beforeCommand_)) {
					commandViewer_.doUpdateHook_ = true;
				}

				if(gui_->showHelp && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Show state as it was before the command");
				}
			}
		}
	}

	// TODO: WIP test
	{
		ImGui::SameLine();
		if(ImGui::Button(ICON_FA_SAVE)) {
			ImGui::OpenPopup("SaveSelection");
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Open save selection dialog");
		}
		if(ImGui::BeginPopup("SaveSelection")) {
			showSavePopup();
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		if(ImGui::Button(ICON_FA_FILE_ALT)) {
			ImGui::OpenPopup("LoadSelection");
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Open load selection popup");
		}
		if(ImGui::BeginPopup("LoadSelection")) {
			showLoadPopup();
			ImGui::EndPopup();
		}
	}

	// == logical state update ==
	auto doUpdate = updateTick_.tick();
	if(doUpdate) {
		auto hookUpdated = selector_.update();
		if(!hookUpdated) {
			// when we would like to have an update per update ticker
			// but there wasn't one right now, try again in every tick
			// until we find one.
			updateTick_.activate();
		} else {
			if(!freezeCommands_) {
				updateFromSelector();
			}

			auto selType = selector_.selectionType();
			if(selType == CommandSelection::SelectionType::command) {
				commandViewer_.updateFromSelector(false);
			}
		}
	}

	// force-update shown batches when it's been too long
	// TODO: do something like that also for non-swapchain mode?
	//   when matching records are submitted but they don't contain the
	//   selected command anymore.
	updateMode = selector_.updateMode();
	if(updateMode == UpdateMode::swapchain && !selector_.freezeState && doUpdate) {
		auto lastPresent = swapchain->frameSubmissions[0].presentID;
		auto statePresent = selector_.hookStateSwapchainPresent();
		if(!selector_.submission() || lastPresent > statePresent + 5) {
			auto diff = lastPresent - statePresent;

			auto selType = selector_.selectionType();
			if(selType == CommandSelection::SelectionType::command) {
				ImGui::SameLine();
				imGuiText("Command not found in {} frames", diff);
				// dlg_trace("force command update");
			}

			// force update
			if(!freezeCommands_) {
				updateRecords(swapchain->frameSubmissions[0].batches,
					{}, {});
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

	if(actionFullscreen_) {
		drawSelected(draw);
	} else {
		// Command list
		auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
		if(!ImGui::BeginTable("RecordViewer", 2, flags, ImGui::GetContentRegionAvail())) {
			return;
		}

		ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, gui_->uiScale() * 250.f);
		ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();

		// ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 20, 20, 255));
		ImGui::BeginChild("Command list", {0, 0});

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, gui_->uiScale() * ImVec2(4.f, 2.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, gui_->uiScale() * ImVec2(4.f, 4.f));

		if(updateMode == UpdateMode::swapchain) {
			displayFrameCommands(*swapchain);
		} else {
			displayRecordCommands();
		}

		ImGui::PopStyleVar(2);

		ImGui::EndChild();
		// ImGui::PopStyleColor();
		ImGui::TableNextColumn();

		drawSelected(draw);

		// command info
		ImGui::EndTable();
	}
}

void CommandRecordGui::drawSelected(Draw& draw) {
	ImGui::BeginChild("Command Info");

	switch(selector_.selectionType()) {
		case SelectionType::none:
			imGuiText("Nothing selected yet");
			break;
		case SelectionType::submission: {
			dlg_assert(selector().updateMode() == UpdateMode::swapchain);
			auto* subm = selector_.submission();
			dlg_assert(subm);
			if(!subm) {
				imGuiText("Error!");
				break;
			}

			refButtonExpect(*gui_, subm->queue);
			imGuiText("submissionID: {}", subm->submissionID);

			if(subm->type == SubmissionType::command) {
				imGuiText("{} records", subm->submissions.size());
			} else {
				imGuiText("{} sparse binds", subm->sparseBinds.size());
			}

			// TODO: more information

			break;
		} case SelectionType::record: {
			auto* rec = selector_.record().get();
			dlg_assert(rec);

			auto hookState = selector_.completedHookState();
			if(hookState) {
				auto lastTime = hookState->neededTime;
				auto validBits = gui_->dev().queueFamilies[rec->queueFamily].props.timestampValidBits;
				if(validBits == 0u) {
					dlg_assert(lastTime == u64(-1));
					imGuiText("Time: unavailable (Queue family does not support timing queries)");
				} else if(lastTime == u64(-1)) {
					dlg_error("lastTime is u64(-1), unexpectedly");
					imGuiText("Time: Error");
				} else {
					auto displayDiff = lastTime * gui_->dev().props.limits.timestampPeriod;
					displayDiff /= 1000.f * 1000.f;
					imGuiText("Time: {} ms", displayDiff);
				}
			} else {
				imGuiText("Time: Waiting for submission...");
			}

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
			commandViewer_.draw(draw, actionFullscreen_);
			break;
	}

	ImGui::EndChild();
}

void CommandRecordGui::select(IntrusivePtr<CommandRecord> record, Command* cmd) {
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
	commandViewer_.updateFromSelector(true);
}

void CommandRecordGui::select(IntrusivePtr<CommandRecord> record, CommandBufferPtr cb) {
	assertNotOwned(gui_->dev().mutex);

	clearSelection(true);
	record_ = std::move(record);
	selector_.select(std::move(cb), record_, {});
	updateFromSelector();
}

void CommandRecordGui::showSwapchainSubmissions(Swapchain& swapchain, bool initial) {
	auto& dev = gui_->dev();
	assertNotOwned(dev.mutex);

	std::vector<FrameSubmission> lastFrame;

	{
		std::lock_guard lock(dev.mutex);
		lastFrame = swapchain.frameSubmissions[0].batches;
	}

	clearSelection(true);
	selector_.select(std::move(lastFrame), u32(-1), nullptr, {});
	updateFromSelector();

	// cool but not a good idea while working on serialization :)
	constexpr auto initialLoadState = true;
	if(initialLoadState && initial) {
		loadStartup();
	}
}

void CommandRecordGui::showLocalCaptures(LocalCapture& lc) {
	assertNotOwned(gui_->dev().mutex);

	clearSelection(true);
	selector_.select(lc);
	updateFromSelector();

	// we want to open all sections up to the selected command
	for(auto* hcmd : command_) {
		dlg_assert(!!hcmd->children() == !!dynamic_cast<const ParentCommand*>(hcmd));
		if(hcmd->children()) {
			openedSections_.insert(static_cast<const ParentCommand*>(hcmd));
		}
	}

	commandViewer_.updateFromSelector(true);
}

void CommandRecordGui::displaySubmission(FrameSubmission& batch, u32 subID) {
	dlg_assert(batch.type == SubmissionType::command);
	dlg_assert(subID < batch.submissions.size());
	auto& rec = batch.submissions[subID];

	// extra tree node for every submission
	const auto id = dlg::format("Commands:{}", subID);
	auto name = "<Unnamed>";
	if(rec->cbName) {
		name = rec->cbName;
	}

	auto flags = ImGuiTreeNodeFlags_FramePadding |
		ImGuiTreeNodeFlags_OpenOnArrow |
		ImGuiTreeNodeFlags_OpenOnDoubleClick |
		ImGuiTreeNodeFlags_SpanFullWidth;

	// check if record was selected
	if(record_ == rec.get() && command_.empty()) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	const auto opened = openedRecords_.count(rec.get());
	ImGui::SetNextItemOpen(opened);

	const auto open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", name);
	if(ImGui::IsItemActivated() && !ImGui::IsItemToggledOpen()) {
		submission_ = &batch;
		record_ = rec;
		command_ = {};

		auto submID = u32(submission_ - frame_.data());
		selector_.select(frame_, submID, record_, command_);
		commandViewer_.unselect();

		// update ops to query time
		CommandHook::Update update;
		update.invalidate = true;
		auto& ops = update.newOps.emplace();
		ops.queryTime = true;
		gui_->dev().commandHook->updateHook(std::move(update));
	}

	// check if open state changed
	if(open && !opened) {
		openedRecords_.insert(rec.get());
	} else if(!open && opened) {
		openedRecords_.erase(rec.get());
	}

	if(!open) {
		return;
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
		commandFlags_ |= Command::Category::end;
	}

	const Command* selectedCommand = nullptr;
	if(!command_.empty()) {
		selectedCommand = command_.back();
	}

	DisplayVisitor visitor(openedSections_, selectedCommand,
		commandFlags_, brokenLabelNesting_);
	visitor.jumpToSelection_ = focusSelected_;
	visitor.showSingleSections_ = showSingleSections_;
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
		commandViewer_.updateFromSelector(true);
	}

	ImGui::Indent(s);
	ImGui::TreePop();
}

void CommandRecordGui::displayBatch(FrameSubmission& batch, u32 batchID) {
	const auto id = dlg::format("submission:{}", batchID);
	auto flags = int(ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanFullWidth);

	// check if this vkQueueSubmit was selected
	if(submission_ == &batch && !record_) {
		dlg_assert(command_.empty());
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	const auto opened = openedSubmissions_.count(&batch);
	ImGui::SetNextItemOpen(opened);

	auto empty = false;
	std::string name;
	if(batch.type == SubmissionType::command) {
		name = "vkQueueSubmit";
		empty = batch.submissions.empty();
	} else if(batch.type == SubmissionType::bindSparse) {
		name = "vkQueueBindSparse";
		empty = true; // this never has children
	} else {
		dlg_error("unreachable");
		name = "???";
	}

	// empty submissions are displayed differently.
	// There is nothing more disappointing than expanding a
	// tree node just to see that it's empty :(
	if(empty) {
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
	} else {
		flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
	}

	if(!batch.queue->name.empty()) {
		name = dlg::format("{}({})", name, batch.queue->name);
	}

	const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", name.c_str());
	if(ImGui::IsItemActivated() && !ImGui::IsItemToggledOpen()) {
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
		return;
	}

	if(!batch.submissions.empty()) {
		ImGui::Separator();
	}

	// we don't want as much space as tree nodes
	auto s = 0.3 * ImGui::GetTreeNodeToLabelSpacing();
	ImGui::Unindent(s);

	if(batch.type == SubmissionType::command) {
		dlg_assert(batch.sparseBinds.empty());
		for(auto r = 0u; r < batch.submissions.size(); ++r) {
			if(r > 0) {
				ImGui::Separator();
			}

			displaySubmission(batch, r);
		}
	} else if(batch.type == SubmissionType::bindSparse) {
		dlg_assert(batch.submissions.empty());
		// no-op
	} else {
		dlg_error("unreachable");
	}

	ImGui::Indent(s);
	ImGui::TreePop();
}

void CommandRecordGui::displayFrameCommands(Swapchain& swapchain) {
	if(frame_.empty() && swapchain.frameSubmissions[0].batches.empty()) {
		dlg_warn("how did this happen?");
		frame_ = swapchain.frameSubmissions[0].batches;
	}

	for(auto b = 0u; b < frame_.size(); ++b) {
		if(b > 0) {
			ImGui::Separator();
		}

		displayBatch(frame_[b], b);
	}
}

void CommandRecordGui::displayRecordCommands() {
	dlg_assert(record_);

	auto* selected = command_.empty() ? nullptr : command_.back();

	if(!brokenLabelNesting_ && record_->brokenHierarchyLabels) {
		brokenLabelNesting_ = true;
		// showing end commands in this viewing mode since
		// they aren't implicitly shown via the nesting
		// now anymore and stuff like EndRenderPass is
		// kinda important.
		commandFlags_ |= Command::Category::end;
	}

	DisplayVisitor visitor(openedSections_,selected,
		commandFlags_, brokenLabelNesting_);
	visitor.jumpToSelection_ = focusSelected_;
	visitor.forbidNewSelection_ = (selector_.updateMode() == UpdateMode::localCapture);
	visitor.showSingleSections_ = showSingleSections_;
	visitor.display(*record_->commands, false);

	auto nsel = std::move(visitor.newSelection_);

	auto newSelection = !nsel.empty() && (
		command_.empty() ||
		nsel.back() != command_.back());
	if(newSelection) {
		dlg_assert(selector_.updateMode() != UpdateMode::localCapture);
		command_ = std::move(nsel);

		// TODO: add cleaner, automatic overload to Selector?
		if(selector_.cb()) {
			selector_.select(
				selector_.cbPtr(),
				record_, command_);
		} else {
			selector_.select(record_, command_);
		}

		commandViewer_.updateFromSelector(true);
	}
}

void CommandRecordGui::clearSelection(bool unselectCommandViewer) {
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
		const CommandSectionMatch& sectionMatch,
		span<const Command*> oldCommand,
		std::vector<const Command*>& newCommand) {

	if(oldSet.count(sectionMatch.a)) {
		newSet.insert(sectionMatch.b);
		transitioned.insert(sectionMatch.a);
	}

	if(!oldCommand.empty() && sectionMatch.a == oldCommand[0]) {
		newCommand.push_back(sectionMatch.b);
		oldCommand = oldCommand.subspan(1u);
	} else {
		oldCommand = {};
	}

	for(auto& child : sectionMatch.children) {
		addMatches(oldSet, newSet, transitioned, child, oldCommand, newCommand);
	}
}

void CommandRecordGui::updateRecords(const FrameMatch& frameMatch,
		std::vector<FrameSubmission>&& records,
		IntrusivePtr<CommandRecord> newRecordGiven,
		std::vector<const Command*> newCommandGiven) {
	ThreadMemScope tms;

	// find matching openedSections_
	// NOTE: with this algorithm we will implicitly close sections
	// that disappear and then re-appear.
	// Not sure how we could keep that information though.
	auto& newOpenSections = tmp_.openedSections;
	auto& newOpenSubmissions = tmp_.openedSubmissions;
	auto& newOpenRecords = tmp_.openedRecords;
	auto& transitionedSections = tmp_.transitionedSections;

	FrameSubmission* newSubmission = nullptr;
	IntrusivePtr<CommandRecord> newRecord {};
	std::vector<const Command*> newCommand {};

	for(auto batchMatch : frameMatch.matches) {
		if(submission_ == batchMatch.a) {
			dlg_assert(!newSubmission);
			newSubmission = const_cast<FrameSubmission*>(batchMatch.b);
		}

		if(openedSubmissions_.count(batchMatch.a)) {
			newOpenSubmissions.insert(batchMatch.b);
			dlg_assert(batchMatch.b - records.data() <= i64(records.size()));
		}

		for(auto& recordMatch : batchMatch.matches) {
			if(record_ == recordMatch.a) {
				dlg_assert(newSubmission);
				dlg_assert(!newRecord);
				newRecord.reset(const_cast<CommandRecord*>(recordMatch.b));
			}

			if(openedRecords_.count(recordMatch.a)) {
				newOpenRecords.insert(recordMatch.b);
			}

			// should only contain the match for the root command, if any
			dlg_assert(recordMatch.matches.size() <= 1);
			if(!recordMatch.matches.empty()) {
				auto prevSize = newCommand.size();
				addMatches(openedSections_, newOpenSections,
					transitionedSections, recordMatch.matches[0],
					command_, newCommand);
				dlg_assert(record_ == recordMatch.a ||
					prevSize == newCommand.size());
			}
		}
	}

	VIL_DEBUG_ONLY(
		for(auto& open : openedSections_) {
			if(!transitionedSections.count(open)) {
				if(auto* lbl = commandCast<const BeginDebugUtilsLabelCmd*>(open); lbl) {
					dlg_trace("Losing open label {}", lbl->name);
				} else {
					dlg_trace("Losing open cmd {}", (const void*) open);
				}
			}
		}
	);

	if(newRecordGiven) {
		dlg_assert(newRecordGiven == newRecord);
		newRecord = newRecordGiven;
	}

	if(!newCommandGiven.empty()) {
		dlg_assert(newCommandGiven.size() == command_.size());
		dlg_assert(newCommand.size() <= command_.size());
		dlg_assert(std::equal(newCommand.begin(), newCommand.end(), newCommandGiven.begin()));
		newCommand = std::move(newCommandGiven);
	} else if(!newCommand.empty() && newCommand.size() < command_.size()) {
		// needed since 'match' can't find the final non-parent-cmd in the
		// hierarchy of command_, have to look that up via find()
		CommandDescriptorSnapshot noDescriptors {};
		auto count = command_.size() - newCommand.size();
		dlg_assert(count == 1u); // should only happen for non-parent selected cmd

		auto dstHierarchy = span(command_).last(count + 1);
		dlg_assert(dstHierarchy[0]->type() == newCommand.back()->type());

		auto fr = find(MatchType::deep,
			*deriveCast<const ParentCommand*>(newCommand.back()),
			dstHierarchy, noDescriptors);

		if(!fr.hierarchy.empty()) {
			dlg_assert(fr.hierarchy.size() == count + 1);
			dlg_assert(fr.hierarchy[0] == newCommand.back());
			newCommand.insert(newCommand.end(),
				fr.hierarchy.begin() + 1, fr.hierarchy.end());
		}
	}

	// apply
	frame_ = std::move(records);

	submission_ = newSubmission;
	record_ = std::move(newRecord);
	command_ = std::move(newCommand);

	std::swap(openedSubmissions_, newOpenSubmissions);
	std::swap(openedRecords_, newOpenRecords);
	std::swap(openedSections_, newOpenSections);

	// clear shouldn't free the memory or shrink the containers
	newOpenSubmissions.clear();
	newOpenRecords.clear();
	newOpenSections.clear();
	transitionedSections.clear();

	if(freezeOnSparseBind_) {
		for(auto& subm : frame_) {
			if(subm.type == SubmissionType::bindSparse) {
				freezeCommands_ = true;
			}
		}
	}
}

void CommandRecordGui::updateRecords(std::vector<FrameSubmission> records,
		IntrusivePtr<CommandRecord> newRecord,
		std::vector<const Command*> newCommand) {
	// update records
	ThreadMemScope tms;
	LinAllocScope localMatchMem(matchAlloc_);
	auto frameMatch = match(tms, localMatchMem, defaultMatchType_, frame_, records);
	updateRecords(frameMatch, std::move(records),
		std::move(newRecord), std::move(newCommand));
}

void CommandRecordGui::updateRecord(IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> newCommandGiven) {

	dlg_assert(record);
	if(!record_) {
		record_ = std::move(record);
		return;
	}

	ThreadMemScope tms;
	LinAllocScope localMatchMem(matchAlloc_);
	auto recMatch = match(tms, localMatchMem, defaultMatchType_,
		*record_->commands, *record->commands);
	std::vector<const Command*> newCommand {};

	auto& newOpenSections = tmp_.openedSections;
	auto& transitionedSections = tmp_.transitionedSections;

	for(auto& sectionMatch : recMatch.children) {
		addMatches(openedSections_, newOpenSections,
			transitionedSections, sectionMatch,
			command_, newCommand);
	}

	VIL_DEBUG_ONLY(
		for(auto& open : openedSections_) {
			if(!transitionedSections.count(open)) {
				if(auto* lbl = commandCast<const BeginDebugUtilsLabelCmd*>(open); lbl) {
					dlg_trace("Losing open label {}", lbl->name);
				} else {
					dlg_trace("Losing open cmd {}", (const void*) open);
				}
			}
		}
	);

	// Would need a custom 'find' call as done in updateRecords in this case.
	dlg_assertm(!newCommandGiven.empty() || command_.empty(),
		"Command finding in single-record mode not implemented");

	if(!newCommandGiven.empty()) {
		dlg_assert(newCommandGiven.size() == command_.size());
		dlg_assert(newCommand.size() <= command_.size());
		dlg_assert(std::equal(newCommand.begin(), newCommand.end(), newCommandGiven.begin()));
		newCommand = std::move(newCommandGiven);
	}

	record_ = std::move(record);
	command_ = std::move(newCommand);

	std::swap(openedSections_, newOpenSections);
	newOpenSections.clear();
	transitionedSections.clear();
}

void CommandRecordGui::updateFromSelector() {
	if(selector_.updateMode() == UpdateMode::swapchain) {
		auto frameSpan = selector_.frame();
		updateRecords(asVector(selector_.frame()),
			selector_.record(), asVector(selector_.command()));

		// TODO: already done in updateRecords, I guess we don't need this
		// here right?
		if(selector_.submission()) {
			auto submID = selector_.submission() - frameSpan.data();
			submission_ = &frame_[submID];
		} else {
			submission_ = nullptr;
		}
	} else {
		updateRecord(selector_.record(), asVector(selector_.command()));
	}
}

const auto serializeFolder = fs::path(".vil/");
constexpr auto serializeFilePrefix = std::string_view("cmdsel_");
constexpr auto serializeDefaultName = std::string_view("_default");
constexpr auto serializeMagicValue = u64(0x411005314A7102BC);

fs::path buildSerializePath(std::string_view name) {
	return serializeFolder / (std::string(serializeFilePrefix).append(name).append(".bin"));
}

std::string readStartup() {
	auto defaultPath = buildSerializePath(serializeDefaultName);
	if(!fs::exists(fs::symlink_status(defaultPath))) {
		return {};
	}

	auto isSymlink = fs::is_symlink(defaultPath);
	if(!isSymlink) {
		dlg_error("{} not a symlink?!", defaultPath);
		return {};
	}

	auto target = fs::read_symlink(defaultPath);
	auto stem = target.stem().u8string();
	if(stem.find(serializeFilePrefix) != 0) {
		dlg_error("Invalid default symlink, target {}", target);
		return {};
	}

	stem.erase(0, serializeFilePrefix.size());
	return stem;
}

void CommandRecordGui::showLoadPopup() {
	auto count = 0u;

	static std::string search;
	imGuiTextInput("Search", search);
	ImGui::Separator();

	if(fs::exists(serializeFolder)) {
		// TODO: ugly
		static auto startupFile = readStartup();

		auto flags = 0u;
		if(ImGui::BeginTable("Entries", 4, flags)) {
			ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("but0", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("but1", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("but2", ImGuiTableColumnFlags_WidthFixed);

			for(const auto& entry : fs::directory_iterator(serializeFolder)) {
				if(entry.is_directory() || entry.is_symlink()) {
					continue;
				}

				auto stem = entry.path().stem().u8string();
				if(stem.find(serializeFilePrefix) != 0) {
					continue;
				}

				stem.erase(0, serializeFilePrefix.size());
				if(!search.empty() && !findSubstrCI(stem, search)) {
					continue;
				}

				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				++count;

				ImGui::AlignTextToFramePadding();
				imGuiText("{}", stem);

				// buttons
				ImGui::TableNextColumn();
				ImGui::PushID(stem.c_str());
				if(ImGui::Button(ICON_FA_FILE_ALT)) {
					loadSelection(stem);
				}
				if(gui_->showHelp && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Load this saved state");
				}

				// startup-button
				ImGui::TableNextColumn();

				auto isDefault = (startupFile == stem);
				pushDisabled(isDefault);

				if(ImGui::Button(ICON_FA_STAR)) {
					auto startupPath = buildSerializePath(serializeDefaultName);
					if(fs::exists(fs::symlink_status(startupPath))) {
						dlg_trace("removed old symlink {}", startupPath);
						fs::remove(startupPath);
					}

					fs::create_symlink(entry.path().filename(), startupPath);
					dlg_trace("created symlink {} -> {}", startupPath, entry.path());

					startupFile = stem;
					dlg_trace("new startup file: {}", stem);
				}

				popDisabled(isDefault);

				if(gui_->showHelp && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					auto str = dlg::format("Load this during startup");
					if(isDefault) {
						str += " (Already active)";
					}

					ImGui::SetTooltip("%s", str.c_str());
				}

				ImGui::PopID();

				ImGui::TableNextColumn();

				if(ImGui::Button(ICON_FA_WINDOW_CLOSE)) {
					fs::remove(entry.path());
				}
				if(gui_->showHelp && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Remove this saved state");
				}
			}

			ImGui::EndTable();
		}
	}

	if(count == 0u) {
		imGuiText("No saved selections found");
	}
}

void CommandRecordGui::showSavePopup() {
	static std::string name;
	auto save = false;
	save |= imGuiTextInput("Save selection as", name, ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	save |= ImGui::Button("Save");

	if(save) {
		saveSelection(name);
	}
}

void CommandRecordGui::loadStartup() {
	auto startupFile = readStartup();
	if(!startupFile.empty()) {
		loadSelection(startupFile);
	}
}

// TODO: proper header:
// - magic value
// - version id

void CommandRecordGui::saveSelection(std::string_view name) {
	if(!fs::exists(serializeFolder)) {
		fs::create_directory(serializeFolder);
	}

	auto path = buildSerializePath(name);

	auto serializerPtr = createStateSaver();
	auto& saver = *serializerPtr;

	DynWriteBuf ownBuf;
	save(saver, ownBuf);

	// write file
	auto binary = true;

	errno = 0;
	auto f = imgio::openFile(path.string().c_str(), binary ? "wb" : "w"); // RAII
	if(!f) {
		dlg_error("Could not open '{}' for writing: {}", path, std::strerror(errno));
		return;
	}

	// TODO: inefficient, additional copy
	DynWriteBuf loaderData;
	auto writer = [&](ReadBuf data) {
		write(loaderData, data);
	};

	write(saver, writer);

	// write file
	DynWriteBuf headerBuf;
	nytl::write(headerBuf, serializeMagicValue);
	nytl::write<u32>(headerBuf, loaderData.size());

	std::fwrite(headerBuf.data(), 1u, headerBuf.size(), f);
	std::fwrite(loaderData.data(), 1u, loaderData.size(), f);
	std::fwrite(ownBuf.data(), 1u, ownBuf.size(), f);

	f = {};
	dlg_trace("saved '{}': loaderSize {}, ownBufSize {}",
		path, loaderData.size(), ownBuf.size());
}

void CommandRecordGui::loadSelection(std::string_view name) {
	// read file
	auto path = buildSerializePath(name);
	if(!fs::exists(path)) {
		dlg_error("'{}' does not exist", path);
		return;
	}

	auto dataVec = imgio::readFile<std::vector<std::byte>>(path.string().c_str());
	if(dataVec.empty()) {
		dlg_error("Error loading {}", path);
		return;
	}

	auto buf = LoadBuf{ReadBuf(dataVec)};

	// read header
	auto magic = read<u64>(buf);
	if(magic != serializeMagicValue) {
		dlg_error("Invalid magic value. Expected {}, got {}",
			serializeMagicValue, magic);
		return;
	}

	auto loaderSize = read<u32>(buf);
	if(loaderSize >= buf.buf.size()) {
		dlg_error("Invalid state file size");
		return;
	}

	auto loaderBuf = buf.buf.subspan(0, loaderSize);
	auto ownBuf = LoadBuf{buf.buf.subspan(loaderSize)};

	dlg_trace("loadState: total {}, loaderSize: {}, ownBufSize: {}",
		dataVec.size(), loaderSize, ownBuf.buf.size());

	try {
		auto loadPtr = createStateLoader(loaderBuf);
		auto& loader = *loadPtr;

		load(loader, ownBuf);
		dlg_assert(ownBuf.buf.empty());
	} catch(const std::exception& err) {
		// TODO: show that in UI
		dlg_error("Error loading {}: {}", path, err.what());
		(void) err;
	}
}

void CommandRecordGui::save(StateSaver& slz, SaveBuf& buf) {
	// selection
	write<u64>(buf, frame_.size());
	for(auto& subm : frame_) {
		write(buf, subm.submissionID);
		write<u64>(buf, subm.submissions.size());

		for(auto& rec : subm.submissions) {
			auto id = add(slz, *rec);
			write<u64>(buf, id);
		}

		// TODO: sparse binds
	}

	auto submID = u32(-1);
	if(submission_) {
		submID = submission_ - frame_.data();
	}
	write(buf, submID);

	auto recID = u64(-1);
	if(record_) {
		recID = add(slz, *record_);
	}
	write(buf, recID);

	write<u64>(buf, command_.size());
	for(auto* cmd : command_) {
		write<u64>(buf, getID(slz, *cmd));
	}

	// opened
	write<u64>(buf, openedSubmissions_.size());
	for(auto* subm : openedSubmissions_) {
		auto submID = subm - frame_.data();
		write<u32>(buf, submID);
	}

	write<u64>(buf, openedRecords_.size());
	for(auto& rec : openedRecords_) {
		auto recID = add(slz, *rec);
		write<u64>(buf, recID);
	}

	write<u64>(buf, openedSections_.size());
	for(auto& cmd : openedSections_) {
		write<u64>(buf, getID(slz, *cmd));
	}

}

// void printMatch(const CommandSectionMatch& m, unsigned indent) {
// 	std::string space;
// 	space.resize(indent, ' ');
//
// 	dlg_trace("{} match {} - {}", space, m.a->toString(), m.b->toString());
// 	for(auto& mm : m.children) {
// 		printMatch(mm, indent + 1);
// 	}
// }

void CommandRecordGui::load(StateLoader& loader, LoadBuf& buf) {
	std::vector<FrameSubmission> frame;
	auto submCount = read<u64>(buf);
	for(auto i = 0u; i < submCount; ++i) {
		auto& subm = frame.emplace_back();
		read(buf, subm.submissionID);

		auto recCount = read<u64>(buf);
		for(auto j = 0u; j < recCount; ++j) {
			auto id = read<u64>(buf);
			subm.submissions.push_back(getRecord(loader, id));
		}

		// TODO: sparse binds
	}

	auto submissionID = read<u32>(buf);

	auto recID = read<u64>(buf);
	auto rec = getRecord(loader, recID);

	std::vector<const Command*> cmd;
	auto cmdCount = read<u64>(buf);
	// dlg_trace(">> Command");
	for(auto i = 0u; i < cmdCount; ++i) {
		auto id = read<u64>(buf);
		auto* c = getCommand(loader, id);
		dlg_assert(c);
		cmd.push_back(c);
		// dlg_trace("  >> {}", c->toString());
	}

	// opened
	openedSubmissions_.clear();
	auto count = read<u64>(buf);
	for(auto i = 0u; i < count; ++i) {
		auto id = read<u32>(buf);
		auto& subm = frame[id];
		openedSubmissions_.insert(&subm);
	}

	openedRecords_.clear();
	count = read<u64>(buf);
	for(auto i = 0u; i < count; ++i) {
		auto id = read<u64>(buf);
		auto rec = getRecord(loader, id);
		openedRecords_.insert(rec.get());
	}

	openedSections_.clear();
	count = read<u64>(buf);
	for(auto i = 0u; i < count; ++i) {
		auto id = read<u64>(buf);
		auto* cmd = getCommand(loader, id);
		dlg_assert(cmd && dynamic_cast<ParentCommand*>(cmd));
		openedSections_.insert(static_cast<ParentCommand*>(cmd));
	}

	// update:
	// first store the loaded state locally but then transition via matching
	// to the actual application's frame
	std::swap(frame_, frame);
	submission_ = submissionID == u32(-1) ? nullptr : &frame_[submissionID];
	record_ = rec;
	command_ = std::move(cmd);

	ThreadMemScope tms;
	LinAllocScope localMatchMem(matchAlloc_);
	auto frameMatch = match(tms, localMatchMem, MatchType::deep, frame_, frame);

	// dlg_trace("frame matches: {}", frameMatch.matches.size());
	// for(auto& m : frameMatch.matches) {
	// 	dlg_trace("rec matches: {}", m.matches.size());
	// 	for(auto& mm : m.matches) {
	// 		dlg_trace("cmd matches: {}", mm.matches.size());
	// 		for(auto& mmm : mm.matches) {
	// 			printMatch(mmm, 1u);
	// 		}
	// 	}
	// }

	updateRecords(frameMatch, std::move(frame), {}, {});

	// also update the selector
	auto submID = 0u;
	if(submission_) {
		submID = u32(submission_ - frame_.data());
	}

	selector_.select(frame_, submID, record_, command_);
	commandViewer_.updateFromSelector(true);
}

} // namespace vil
