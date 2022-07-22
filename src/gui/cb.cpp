#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <gui/util.hpp>
#include <gui/fontAwesome.hpp>
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

struct DisplayVisitor : CommandVisitor {
	std::unordered_set<const ParentCommand*>& openedSections_;

	const Command* sel_;
	Command::TypeFlags flags_;
	std::vector<const Command*> newSelection_;

	// whether to use broken-hierarchy mode
	bool labelOnlyIndent_ {};
	bool open_ {true};
	u32 indent_ {};

	bool jumpToSelection_ {};

	DisplayVisitor(std::unordered_set<const ParentCommand*>& opened,
			const Command* sel, Command::TypeFlags flags, bool labelOnlyIndent) :
		openedSections_(opened), sel_(sel), flags_(flags), labelOnlyIndent_(labelOnlyIndent) {
	}

	~DisplayVisitor() {
		finish();
	}

	// Close any remaining trees in the end
	void finish() {
		if(!labelOnlyIndent_) {
			return;
		}

		while(indent_ > 0) {
			--indent_;
			ImGui::TreePop();
		}
	}

	auto getUnindent() {
		return 0.3 * ImGui::GetTreeNodeToLabelSpacing();
	}

	// Returns whether one of the children was selected
	bool displayCommands(const Command* cmd, bool firstSep) {
		// TODO PERF: should use imgui list clipper, might have *a lot* of commands here.
		// But first we have to restrict what cmd->display can actually do.
		// Would also have to pre-filter commands for that. And stop at every
		// (expanded) parent command (but it's hard to tell whether they are
		// expanded).
		std::vector<const Command*> ret;
		auto showSep = firstSep;
		auto emptyBefore = newSelection_.empty();
		while(cmd) {

			// No matter the flags, we never want to hide parent commands.
			if(open_ && ((flags_ & cmd->type()) || cmd->children())) {
				if(showSep) {
					ImGui::Separator();
				}

				cmd->visit(*this);
				showSep = true;
			} else {
				// still visit it, for indentation tracking
				// only relevant for EndDebugUtilsLabelCmd commands
				cmd->visit(*this);
			}

			cmd = cmd->next;
		}

		auto emptyAfter = newSelection_.empty();
		dlg_assert(!(!emptyBefore && emptyAfter));
		return emptyBefore && !emptyAfter;
	}

	bool displayChildren(const ParentCommand& cmd, const Command* children, bool sep = true) {
		auto selected = displayCommands(children, sep);
		if(selected) {
			dlg_assert(!newSelection_.empty());
			newSelection_.insert(newSelection_.begin(), &cmd);
		}

		return selected;
	}

	bool displayChildren(const ParentCommand& cmd, bool sep = true) {
		return displayChildren(cmd, cmd.children(), sep);
	}

	// Returns whether the tree is open
	bool openTree(const ParentCommand& cmd) {
		int flags = ImGuiTreeNodeFlags_OpenOnArrow |
			ImGuiTreeNodeFlags_FramePadding |
			ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if(sel_ == &cmd) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		const auto opened = openedSections_.count(&cmd);
		ImGui::SetNextItemOpen(opened);

		const auto open = ImGui::TreeNodeEx(static_cast<const void*>(&cmd),
			flags, "%s", cmd.toString().c_str());

		// don't select when only clicked on arrow
		const auto labelStartX = ImGui::GetItemRectMin().x + 30;
		if(ImGui::IsItemClicked() && ImGui::GetMousePos().x > labelStartX) {
			dlg_assert(newSelection_.empty());
			newSelection_.push_back(&cmd);
		}

		// check if open
		if(open && !opened) {
			openedSections_.insert(&cmd);
		} else if(!open && opened) {
			openedSections_.erase(&cmd);
		}

		return open;
	}

	// Returns whether something was selected
	bool displayOpen(const ParentCommand& cmd, const Command* children) {
		dlg_assert(!labelOnlyIndent_);

		auto open = openTree(cmd);
		if(!open) {
			return false;
		}

		auto ret = false;
		if(children) {
			// we don't want as much space as tree nodes
			auto s = getUnindent();
			ImGui::Unindent(s);
			ret = displayChildren(cmd, children);
			ImGui::Indent(s);
		}

		ImGui::TreePop();
		return ret;
	}

	void display(const Command& cmd) {
		if(!(flags_ & cmd.type()) || !open_) {
			return;
		}

		int flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
			ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;
		if(sel_ == &cmd) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		ImGui::TreeNodeEx(static_cast<const void*>(&cmd),
			flags, "%s", cmd.toString().c_str());

		auto clicked = ImGui::IsItemClicked();
		if(clicked) {
			dlg_assert(newSelection_.empty());
			newSelection_.push_back(&cmd);
		}

		if(jumpToSelection_ && (sel_ == &cmd || clicked)) {
			ImGui::SetScrollHereY(0.5f);
		}
	}

	void displayAndChildren(const ParentCommand& cmd) {
		dlg_assert(labelOnlyIndent_);
		display(cmd);
		displayChildren(cmd);
	}

	// == Visitor functions ==
	void visit(const Command& cmd) override {
		display(cmd);
	}

	void visit(const ParentCommand& cmd) override {
		if(labelOnlyIndent_) {
			displayAndChildren(cmd);
		} else {
			displayOpen(cmd, cmd.children());
		}
	}

	void visit(const BeginDebugUtilsLabelCmd& cmd) override {
		if(labelOnlyIndent_) {
			if(!open_) {
				return;
			}

			open_ = openTree(cmd);

			if(open_) {
				++indent_;
				ImGui::Unindent(getUnindent());
			}

			displayChildren(cmd);
		} else {
			displayOpen(cmd, cmd.children());
		}
	}

	void visit(const EndDebugUtilsLabelCmd& cmd) override {
		display(cmd);

		if(labelOnlyIndent_ && indent_ > 0 && open_) {
			--indent_;
			ImGui::Indent(getUnindent());
			ImGui::TreePop();
		}

		open_ = true;
	}

	void visit(const BeginRenderPassCmd& cmd) override {
		if(labelOnlyIndent_) {
			displayAndChildren(cmd);
		} else {
			auto children = cmd.children();
			auto first = static_cast<FirstSubpassCmd*>(nullptr);
			if(children) {
				// If we only have one subpass, don't give it an extra section
				// to make everything more compact.
				first = dynamic_cast<FirstSubpassCmd*>(children);
				dlg_assert(first);
				if(!first->next) {
					children = first->children_;
				}
			}

			auto selected = displayOpen(cmd, children);

			// if we showed alternative children, we have to patch
			// the selection hierarchy here
			if(selected && children != cmd.children_) {
				dlg_assert(!newSelection_.empty());
				newSelection_.insert(newSelection_.begin() + 1, first);
			}
		}
	}

	void visit(const FirstSubpassCmd& cmd) override {
		if(labelOnlyIndent_) {
			// skip display of this command itself.
			displayChildren(cmd, false);
		} else {
			visit(static_cast<const SectionCommand&>(cmd));
		}
	}

	void visit(const ExecuteCommandsCmd& cmd) override {
		// TODO: can lables really pass CmdExecuteCommands boundaries?
		// if not, we can always indent here as well.
		if(labelOnlyIndent_) {
			// TODO: this will result in CmdExecuteCommands being shown
			// and then the commands, with same indentation level.
			// This is kinda confusing but not sure how to improve.
			// Just hiding the CmdExecuteCommands is problematic since
			// it contains additional information.
			displayAndChildren(cmd);
		} else {
			auto children = cmd.children();
			auto first = static_cast<ExecuteCommandsChildCmd*>(nullptr);
			if(children) {
				// If we only have one command buffer, don't give it an extra section
				// to make everything more compact.
				first = dynamic_cast<ExecuteCommandsChildCmd*>(children);
				dlg_assert(first);
				if(!first->next) {
					children = first->record_->commands;
				}
			}

			auto selected = displayOpen(cmd, children);

			// if we showed alternative children, we have to patch
			// the selection hierarchy here
			if(selected && children != cmd.children_) {
				dlg_assert(!newSelection_.empty());
				newSelection_.insert(newSelection_.begin() + 1, first);
			}
		}
	}
};

// CommandBufferGui
void CommandBufferGui::init(Gui& gui) {
	gui_ = &gui;
	commandFlags_ = CommandType(~(CommandType::end | CommandType::bind | CommandType::query));
	commandViewer_.init(gui);
}

CommandBufferGui::~CommandBufferGui() = default;

void CommandBufferGui::draw(Draw& draw) {
	ZoneScoped;

	if(!record_ && mode_ != UpdateMode::swapchain) {
		ImGui::Text("No record selected");
		return;
	}

	auto& dev = gui_->dev();
	auto& hook = *dev.commandHook;

	// Possibly update mode
	auto modeName = [](UpdateMode mode) {
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

	if(mode_ != UpdateMode::swapchain) {
		// only show combo if at least one update option is available
		auto showCombo = (cb_ || record_->cb);

		if(showCombo && ImGui::BeginCombo("Update Source", modeName(mode_))) {
			if(ImGui::Selectable("None")) {
				mode_ = UpdateMode::none;
				hook.target = {};
				hook.target.record = record_.get();
			}

			if(record_ && ImGui::Selectable("Any")) {
				mode_ = UpdateMode::any;
				hook.target = {};
				hook.target.all = true;
			}

			if((mode_ == UpdateMode::commandBuffer && cb_) || record_->cb) {
				if(ImGui::Selectable("CommandBuffer")) {
					if(!cb_) {
						cb_ = record_->cb;
					}

					mode_ = UpdateMode::commandBuffer;
					hook.target = {};
					hook.target.cb = cb_;
				}
			}

			ImGui::EndCombo();
		}
	} else if(!gui_->dev().swapchain) {
		clearSelection();

		records_ = {};
		openedSections_ = {};
		swapchainPresent_ = {};
		return;
	}

	// Settings
	if(ImGui::Button(ICON_FA_WRENCH)) {
		ImGui::OpenPopup("Command Selector");
	}

	if(ImGui::BeginPopup("Command Selector")) {
		auto val = commandFlags_.value();

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
	// when we are viewing an invalidated record without updating.
	if(ImGui::Checkbox("Freeze State", &freezeState_)) {
		hook.freeze = freezeState_;
	}

	if(gui_->showHelp && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("This will freeze the state of the command you are viewing,\n"
			"e.g. the image/buffer content and measured time.\n"
			"This does not affect updating of the commands shown on the left.");
	}

	ImGui::SameLine();
	auto doUpdate = updateTick_.tick();

	if(mode_ != UpdateMode::none) {
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

	// force-update shown batches when it's been too long
	if(mode_ == UpdateMode::swapchain && !freezeState_ && doUpdate) {
		auto lastPresent = dev.swapchain->frameSubmissions[0].presentID;
		if(record_ && lastPresent > swapchainPresent_ + 4) {
			auto diff = lastPresent - swapchainPresent_;

			ImGui::SameLine();
			imGuiText("Command not found in {} frames", diff);

			// force update
			if(!freezeCommands_) {
				dlg_trace("force command update");
				updateRecords(dev.swapchain->frameSubmissions[0].batches, false);
			}
		}
	}

	// TODO: do we really want/need this?
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

	if(mode_ == UpdateMode::swapchain) {
		displayFrameCommands();
	} else {
		displayRecordCommands();
	}

	ImGui::PopStyleVar(2);

	// logical state update
	if(doUpdate) {
		updateState();
	}

	// always clear the completed hooks, no matter if we update
	// the state or not.
	hook.completed.clear();

	ImGui::EndChild();
	// ImGui::PopStyleColor();
	ImGui::TableNextColumn();

	// command info
	ImGui::BeginChild("Command Info");

	switch(selectionType_) {
		case SelectionType::none:
			imGuiText("Nothing selected yet");
			break;
		case SelectionType::submission:
			dlg_assert(mode_ == UpdateMode::swapchain);
			dlg_assert(selectedBatch_);
			if(!selectedBatch_) {
				imGuiText("Error!");
				break;
			}

			refButtonExpect(*gui_, selectedBatch_->queue);
			imGuiText("{} records", selectedBatch_->submissions.size());
			imGuiText("submissionID: {}", selectedBatch_->submissionID);
			break;
		case SelectionType::record:
			dlg_assert(selectedRecord_);
			refButtonD(*gui_, selectedRecord_->cb);
			imGuiText("cb name: {}", selectedRecord_->cbName);
			imGuiText("num handles: {}", selectedRecord_->handles.size());
			imGuiText("broken labels: {}", selectedRecord_->brokenHierarchyLabels);
			imGuiText("record id: {}", selectedRecord_->recordID);
			imGuiText("refCount: {}", selectedRecord_->refCount);
			imGuiText("num hook records: {}", selectedRecord_->hookRecords.size());
			break;
		case SelectionType::command:
			commandViewer_.draw(draw);
			break;
	}

	ImGui::EndChild();
	ImGui::EndTable();
}

// TODO: some code duplication here...
void CommandBufferGui::select(IntrusivePtr<CommandRecord> record) {
	mode_ = UpdateMode::none;
	cb_ = {};

	// NOTE: we could try to find new command matching old description.
	// if(record_ && !desc_.empty()) {
	// 	command_ = CommandDesc::find(record_->commands, desc_);
	// 	// update hooks here
	// }

	// Unset hooks
	clearSelection();
	record_ = std::move(record);
	openedSections_ = {};
	updateHookTarget();
}

void CommandBufferGui::select(IntrusivePtr<CommandRecord> record,
		CommandBuffer& cb) {
	mode_ = UpdateMode::commandBuffer;
	cb_ = &cb;

	// NOTE: we could try to find new command matching old description.
	// if(record_ && !desc_.empty()) {
	// 	command_ = CommandDesc::find(record_->commands, desc_);
	// 	// update hooks here
	// }

	// Unset hooks
	clearSelection();
	record_ = std::move(record);
	openedSections_ = {};
	updateHookTarget();
}

void CommandBufferGui::showSwapchainSubmissions() {
	mode_ = UpdateMode::swapchain;
	cb_ = {};
	clearSelection();

	// NOTE: don't do that here, we explicitly preserve them.
	// openedSections_ = {};
}

void CommandBufferGui::destroyed(const Handle& handle) {
	(void) handle;

	if(mode_ == UpdateMode::commandBuffer) {
		dlg_assert(cb_ && record_);
		if(cb_ == &handle) {
			cb_ = nullptr;
			mode_ = UpdateMode::none;

			auto& hook = *gui_->dev().commandHook;
			dlg_assert(!hook.target.cb || hook.target.cb == &handle);
			hook.target = {};
		}
	}

	// otherwise we don't care as we only deal with recordings that have shared
	// ownership, i.e. are kept alive by us.
}

void CommandBufferGui::updateHookTarget() {
	auto& hook = *gui_->dev().commandHook;
	hook.target = {};

	switch(mode_) {
		case UpdateMode::none:
			hook.target.record = record_.get();
			break;
		case UpdateMode::any:
		case UpdateMode::swapchain:
			hook.target.all = true;
			break;
		case UpdateMode::commandBuffer:
			hook.target.cb = cb_;
			break;
	}
}

void CommandBufferGui::updateState() {
	auto& dev = gui_->dev();
	auto& hook = *dev.commandHook;

	// TODO: correctly implement for swapchain + batch/record selection.
	// Just match with all (or just the latest?) finished frame and try
	// to find equivalent there, updating record_.

	// try to update the shown commands with the best new match
	if(!hook.completed.empty() && (!freezeState_ || !commandViewer_.state())) {
		// find the best match
		CommandHook::CompletedHook* best = nullptr;
		auto bestMatch = 0.f;
		MatchResult bestMatchResult {};
		u32 bestPresentID = {};
		span<const FrameSubmission> bestBatches; // only for swapchain mode

		for(auto& res : hook.completed) {
			float resMatch = res.match;
			span<const FrameSubmission> foundBatches;
			u32 presentID {};

			// When we are in swapchain mode, we need the frame associated with
			// this submission. We will then also consider the submission in its
			// context inside the frame.
			MatchResult batchMatches;
			if(mode_ == UpdateMode::swapchain) {
				dlg_assert(selectionType_ == SelectionType::command);

				for(auto& frame : gui_->dev().swapchain->frameSubmissions) {
					if(res.submissionID >= frame.submissionStart && res.submissionID <= frame.submissionEnd) {
						foundBatches = frame.batches;
						presentID = frame.presentID;
						break;
					}
				}

				// when the hook is from too long ago, we won't
				if(foundBatches.empty()) {
					dlg_warn("Couldn't find frame associated to hooked submission");
					continue;
				}

				dlg_assert(!selectedFrame_.empty());

				// check if [selectedRecord_ in selectedFrame_] contextually
				// matches  [res.record in foundBatches]
				ThreadMemScope tms;
				batchMatches = match(tms, selectedFrame_, foundBatches);
				bool recordMatched = false;
				for(auto& batchMatch : batchMatches.matches) {
					if(batchMatch.b->submissionID != res.submissionID) {
						continue;
					}

					for(auto& recMatch : batchMatch.matches) {
						if(recMatch.b != res.record) {
							continue;
						}

						if(recMatch.a == selectedRecord_.get()) {
							recordMatched = true;
							resMatch *= recMatch.match;

							// TODO: For all parent commands, we could make
							// sure that they match.
							// I.e. (command_[i], res.command[i]) somewhere
							// in the recMatch.matches[X]...matches hierarchy
						}

						break;
					}

					break;
				}

				// In this case, the newly hooked potentialy candidate did
				// not match with our previous record in content; we won't use it.
				if(!recordMatched) {
					dlg_info("Hooked record did not match. Total match: {}, match count {}",
						batchMatches.match, batchMatches.matches.size());
					continue;
				}
			}

			if(resMatch > bestMatch) {
				best = &res;
				bestMatch = resMatch;
				bestBatches = foundBatches;
				bestPresentID = presentID;
				bestMatchResult = batchMatches;
			}
		}

		if(best) {
			// we alwyas update the commandHook desc (even if we don't
			// update the internal state below) since this record is newer
			// than the old one and might have valid handles/state that is
			// already unset in the old one.
			dev.commandHook->desc(best->record, best->command,
				best->descriptorSnapshot, false);

			// Update internal state state from hook match
			// In swapchain mode - and when not freezing commands - make
			// sure to also display the new frame
			if(mode_ == UpdateMode::swapchain) {
				swapchainPresent_ = bestPresentID;

				if(!freezeCommands_) {
					// TODO PERF: can re-use bestMatchResult in many cases,
					updateRecords({bestBatches.begin(), bestBatches.end()}, false);
					selectedFrame_ = {bestBatches.begin(), bestBatches.end()};
					selectedCommand_ = best->command;
					selectedRecord_ = best->record;
					record_ = best->record;
				}
			} else if(!freezeCommands_) {
				selectedCommand_ = best->command;
				updateRecord(best->record);
			}

			// update command viewer state from hook match
			// TODO: this call messes with the commandHook, potentially
			// invalidated 'best'. And messing with freeze. Should
			// clean up responsibilities here. If we don't have
			// to call this here, we could also move from the vectors
			// from *best above.
			commandViewer_.select(best->record, *best->command.back(),
				best->descriptorSnapshot, false, best->state);
			hook.stillNeeded = best->state.get();

			// in this case, we want to freeze state but temporarily
			// unfroze it to get a new CommandHookState. This happens
			// e.g. when selecting a new command or viewed resource
			if(!hook.freeze && freezeState_) {
				hook.freeze = true;
			}
		}
	}

	// When no command was selected yet, we won't get any new records
	// from CommandHook. But still want to show the new commands.
	if(!freezeCommands_ && selectedCommand_.empty()) {
		if(mode_ == UpdateMode::swapchain) {
			dlg_assert(selectionType_ != SelectionType::command);
			updateRecords(gui_->dev().swapchain->frameSubmissions[0].batches, true);
			swapchainPresent_ = gui_->dev().swapchain->frameSubmissions[0].presentID;
		} else if(mode_ == UpdateMode::commandBuffer) {
			dlg_assert(cb_);
			updateRecord(cb_->lastRecordPtrLocked());
		} else if(mode_ == UpdateMode::any) {
			// TODO: correct updating, iterate through all submissions
			// in the last frames and find the best record match. ouch.
			dlg_error("TODO: shown commands not correctly updated until selected");
		}
	}
}

void CommandBufferGui::displayFrameCommands() {
	auto& dev = gui_->dev();

	if(records_.empty() && !gui_->dev().swapchain->frameSubmissions.empty()) {
		records_ = gui_->dev().swapchain->frameSubmissions[0].batches;
	}

	const Command* selectedCommand = nullptr;
	if(selectionType_ == SelectionType::command) {
		dlg_assert(record_);
		dlg_assert(!selectedCommand_.empty());
		selectedCommand = selectedCommand_.back();
	}

	for(auto b = 0u; b < records_.size(); ++b) {
		auto& batch = records_[b];
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
		if(selectionType_ == SelectionType::submission && selectedBatch_ &&
				// TODO: this comparison is kinda hacky.
				// batch comes from records_ while selectedBatch_ comes
				// from selectedFrame_, that's why we can't compare pointers.
				selectedBatch_->submissionID == batch.submissionID) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "vkQueueSubmit");
		const auto labelStartX = ImGui::GetItemRectMin().x + 30;
		if(ImGui::IsItemClicked() && ImGui::GetMousePos().x > labelStartX) {
			clearSelection();
			selectionType_ = SelectionType::submission;
			selectedFrame_ = records_;
			selectedBatch_ = &selectedFrame_[b];
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

			// When the record isn't valid anymore (cb is unset), we have to
			// be careful to not actually reference any destroyed resources.
			if(!rec->cb) {
				replaceInvalidatedLocked(*rec);
			}

			dlg_assert(rec->invalidated.empty());

			// extra tree node for every submission
			const auto id = dlg::format("Commands:{}", r);
			auto name = "<Unnamed>";
			if(rec->cbName) {
				name = rec->cbName;
			}

			auto flags = ImGuiTreeNodeFlags_FramePadding |
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_OpenOnDoubleClick;
			if(selectionType_ == SelectionType::record && selectedRecord_ == rec) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			const auto open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", name);
			const auto labelStartX = ImGui::GetItemRectMin().x + 30;
			if(ImGui::IsItemClicked() && ImGui::GetMousePos().x > labelStartX) {
				clearSelection();
				selectionType_ = SelectionType::record;
				selectedRecord_ = rec;
				selectedFrame_ = records_;
				selectedBatch_ = &selectedFrame_[b];
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
			visitor.displayCommands(rec->commands->children_, true);
			auto nsel = std::move(visitor.newSelection_);

			auto newSelection = !nsel.empty() && (
				selectedCommand_.empty() ||
				nsel.back() != selectedCommand_.back());
			if(newSelection) {
				clearSelection();

				selectionType_ = SelectionType::command;
				selectedCommand_ = std::move(nsel);
				selectedRecord_ = rec;
				selectedFrame_ = records_;
				selectedBatch_ = &selectedFrame_[b];
				record_ = rec;

				// TODO: do full snapshot in hook and use that here (if we already have a completed hook)?
				//  descriptors might already be destroyed here, snapshotRelevantDescriptors won't help
				//  maybe make that an option via gui or something
				auto dsSnapshot = snapshotRelevantDescriptorsLocked(*selectedCommand_.back());
				commandViewer_.select(selectedRecord_, *selectedCommand_.back(),
					dsSnapshot, true, nullptr);

				dev.commandHook->target = {};
				dev.commandHook->target.all = true;
				dev.commandHook->desc(selectedRecord_, selectedCommand_, dsSnapshot);
				dev.commandHook->freeze = false;
				dev.commandHook->stillNeeded = nullptr;
			}

			ImGui::Indent(s);
			ImGui::TreePop();
		}

		ImGui::Indent(s);
		ImGui::TreePop();
	}
}

void CommandBufferGui::displayRecordCommands() {
	auto& dev = gui_->dev();
	dlg_assert(record_);

	// When the record isn't valid anymore (cb is unset), we have to
	// be careful to not actually reference any destroyed resources.
	if(record_ && !record_->cb) {
		replaceInvalidatedLocked(*record_);
	}

	dlg_assert(record_->invalidated.empty());

	auto* selected = selectedCommand_.empty() ? nullptr : selectedCommand_.back();

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
	visitor.displayCommands(record_->commands->children_, false);
	auto nsel = std::move(visitor.newSelection_);

	auto newSelection = !nsel.empty() && (
		selectedCommand_.empty() ||
		nsel.back() != selectedCommand_.back());
	if(newSelection) {
		if(mode_ == UpdateMode::none) {
			dlg_assert(dev.commandHook->target.record == record_.get());
		} else if(mode_ == UpdateMode::commandBuffer) {
			dlg_assert(cb_);
			dlg_assert(dev.commandHook->target.cb == cb_);
		} else if(mode_ == UpdateMode::any) {
			dlg_assert(dev.commandHook->target.all);
		}

		selectionType_ = SelectionType::command;
		selectedCommand_ = std::move(nsel);
		selectedRecord_ = record_;

		// TODO: do full snapshot in hook and use that here (if we already have a completed hook)?
		//  descriptors might already be destroyed, snapshotRelevantDescriptors won't help
		//  maybe make that an option via gui or something
		auto dsSnapshot = snapshotRelevantDescriptorsLocked(*selectedCommand_.back());
		commandViewer_.select(selectedRecord_, *selectedCommand_.back(),
			dsSnapshot, true, nullptr);

		// in any case, update the hook
		dev.commandHook->desc(selectedRecord_, selectedCommand_, dsSnapshot);
		dev.commandHook->freeze = false;
		dev.commandHook->stillNeeded = nullptr;
	}
}

void CommandBufferGui::clearSelection() {
	auto& dev = gui_->dev();
	commandViewer_.unselect();

	selectionType_ = SelectionType::none;
	selectedFrame_ = {};
	selectedBatch_ = nullptr;
	selectedRecord_ = {};
	selectedCommand_ = {};

	dev.commandHook->target = {};
	dev.commandHook->unsetHookOps();
	dev.commandHook->desc({}, {}, {});
	dev.commandHook->stillNeeded = nullptr;

	if (mode_ == UpdateMode::swapchain) {
		record_ = {};
	}
}

void CommandBufferGui::updateRecords(std::vector<FrameSubmission> records,
		bool updateSelection) {
	// update records
	{
		ThreadMemScope tms;
		auto frameMatch = match(tms, records_, records);
		updateRecords(frameMatch, records);
	}

	// update selection
	if(selectionType_ == SelectionType::none) {
		dlg_assert(!selectedBatch_);
		dlg_assert(!selectedRecord_);
		updateSelection = false;
	}

	if(!updateSelection) {
		return;
	}

	dlg_assert(selectionType_ != SelectionType::command);

	// TODO PERF: can re-use frameMatch in many cases,
	ThreadMemScope tms;
	auto selMatch = match(tms, selectedFrame_, records);

	std::optional<u32> newSelectedBatch = {};
	IntrusivePtr<CommandRecord> newSelectedRecord = {};

	for(auto batchMatch : selMatch.matches) {
		if(updateSelection && selectedBatch_ == batchMatch.a) {
			dlg_assert(!newSelectedBatch);

			// TODO: kinda hacky. But needed since the FrameSubmission
			// references in frameMatch might not directly reference
			// into records
			auto cmp = [&](const FrameSubmission& rec) {
				return rec.submissionID == batchMatch.b->submissionID;
			};
			auto it = find_if(records, cmp);
			dlg_assert(it != records.end());
			newSelectedBatch = u32(it - records.begin());
		}

		for(auto& recordMatch : batchMatch.matches) {
			if(updateSelection && selectedRecord_ == recordMatch.a) {
				newSelectedRecord.reset(const_cast<CommandRecord*>(recordMatch.b));
			}
		}
	}

	if(selectionType_ == SelectionType::submission) {
		updateSelection = updateSelection && !!newSelectedBatch;
	} else if(selectionType_ == SelectionType::record) {
		updateSelection = updateSelection && !!newSelectedBatch && !!newSelectedRecord;
	}

	if(updateSelection) {
		selectedFrame_ = records;
		selectedBatch_ = &selectedFrame_[*newSelectedBatch];
		selectedRecord_ = std::move(newSelectedRecord);
	}
}

// util
// The recursion here can be assumed safe, applications nesting many thousand
// levels of sections (only possible with debug labels i guess) are weird
// and we don't need to support them at the moment.
void addMatches(
		const std::unordered_set<const ParentCommand*>& oldSet,
		std::unordered_set<const ParentCommand*>& newSet,
		std::unordered_set<const ParentCommand*>& transmitted,
		const SectionMatch& sectionMatch) {

	if(oldSet.count(sectionMatch.a)) {
		newSet.insert(sectionMatch.b);
		transmitted.insert(sectionMatch.a);
	}

	for(auto& child : sectionMatch.children) {
		addMatches(oldSet, newSet, transmitted, child);
	}
}

void CommandBufferGui::updateRecords(const MatchResult& frameMatch,
		std::vector<FrameSubmission> records) {
	ThreadMemScope tms;

	// find matching openedSections_
	// NOTE: with this algorithm we will implicitly close sections
	// that disappear and then re-appear.
	// Not sure how we could keep that information though.
	std::unordered_set<const ParentCommand*> newOpen;
	std::unordered_set<const ParentCommand*> transmitted;

	for(auto batchMatch : frameMatch.matches) {
		for(auto& recordMatch : batchMatch.matches) {
			for(auto& sectionMatch : recordMatch.matches) {
				addMatches(openedSections_, newOpen, transmitted, sectionMatch);
			}
		}
	}

	// TODO PERF debugging, remove otherwise
	for(auto& open : openedSections_) {
		if(!transmitted.count(open)) {
			if(auto* lbl = dynamic_cast<const BeginDebugUtilsLabelCmd*>(open); lbl) {
				dlg_trace("Losing open label {}", lbl->name);
			} else {
				dlg_trace("Losing open cmd {}", (const void*) open);
			}
		}
	}

	// apply
	records_ = std::move(records);
	openedSections_ = std::move(newOpen);
}

void CommandBufferGui::updateRecord(IntrusivePtr<CommandRecord> record) {
	dlg_trace("TODO");
	record_ = std::move(record);
}

} // namespace vil
