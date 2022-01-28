#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <gui/util.hpp>
#include <queue.hpp>
#include <ds.hpp>
#include <swapchain.hpp>
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
	const Command* sel_;
	Command::TypeFlags flags_;
	std::vector<const Command*> newSelection_;

	// whether to use broken-hierarchy mode
	bool labelOnlyIndent_ {};
	bool open_ {true};
	u32 indent_ {};

	DisplayVisitor(const Command* sel, Command::TypeFlags flags, bool labelOnlyIndent) :
		sel_(sel), flags_(flags), labelOnlyIndent_(labelOnlyIndent) {
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
	bool openTree(const Command& cmd) {
		int flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;
		if(sel_ == &cmd) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		auto idStr = dlg::format("{}:{}", cmd.nameDesc(), cmd.relID);
		auto open = ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", cmd.toString().c_str());
		if(ImGui::IsItemClicked()) {
			// don't select when only clicked on arrow
			if(ImGui::GetMousePos().x > ImGui::GetItemRectMin().x + 30) {
				dlg_assert(newSelection_.empty());
				newSelection_.push_back(&cmd);
			}
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

		auto idStr = dlg::format("{}:{}", cmd.nameDesc(), cmd.relID);
		ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", cmd.toString().c_str());

		if(ImGui::IsItemClicked()) {
			dlg_assert(newSelection_.empty());
			newSelection_.push_back(&cmd);
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
		auto showCombo =
			// (record_ && record_->group) ||
			(cb_ || record_->cb);

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
		record_ = {};
		selectedRecord_ = {};
		records_ = {};
		selectedBatch_ = {};
		swapchainPresent_ = {};
		command_ = {};
		hook.target = {};
		hook.desc({}, {}, {});
		hook.unsetHookOps();
		return;
	}

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

	// Selector for visible commands
	ImGui::SameLine();
	if(ImGui::Button("Settings")) {
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

		ImGui::EndPopup();
	}

	// force-update shown batches when it's been too long
	if(mode_ == UpdateMode::swapchain && !freezeState_) {
		auto lastPresent = dev.swapchain->frameSubmissions[0].presentID;
		if(record_ && lastPresent > swapchainPresent_ + 4) {
			auto diff = lastPresent - swapchainPresent_;

			ImGui::SameLine();
			imGuiText("Command not found in {} frames", diff);

			// force update
			if(!freezeCommands_) {
				records_ = dev.swapchain->frameSubmissions[0].batches;
				// record_ = {};
				// command_ = {};
			}
		}
	}

	if(mode_ == UpdateMode::none) {
		imGuiText("Showing static record");
	} else if(mode_ == UpdateMode::commandBuffer) {
		dlg_assert(!record_->cb || record_->cb == cb_);
		dlg_assert(cb_ && cb_->lastRecordPtrLocked());

		imGuiText("Updating from Command Buffer");
		ImGui::SameLine();
		refButton(*gui_, *cb_);
	} else if(mode_ == UpdateMode::any) {
		imGuiText("Updating from any records");
	} else if(mode_ == UpdateMode::swapchain) {
		auto& sc = *gui_->dev().swapchain;

		imGuiText("Showing per-present commands from");
		ImGui::SameLine();
		refButton(*gui_, sc);

	}

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
		if(records_.empty() && !gui_->dev().swapchain->frameSubmissions.empty()) {
			records_ = gui_->dev().swapchain->frameSubmissions[0].batches;
		}

		auto* selected = record_ && !command_.empty() ? command_.back() : nullptr;

		for(auto b = 0u; b < records_.size(); ++b) {
			auto& batch = records_[b];
			auto id = dlg::format("vkQueueSubmit:{}", b);

			auto flags = int(ImGuiTreeNodeFlags_FramePadding);
			if(batch.submissions.empty()) {
				flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
			}

			if(ImGui::TreeNodeEx(id.c_str(), flags, "vkQueueSubmit")) {
				if(!batch.submissions.empty()) {
					ImGui::Separator();
				}

				// we don't want as much space as tree nodes
				auto s = 0.3 * ImGui::GetTreeNodeToLabelSpacing();
				ImGui::Unindent(s);

				for(auto r = 0u; r < batch.submissions.size(); ++r) {
					auto& rec = batch.submissions[r];

					// When the record isn't valid anymore (cb is unset), we have to
					// be careful to not actually reference any destroyed resources.
					if(!rec->cb) {
						replaceInvalidatedLocked(*rec);
					}

					dlg_assert(rec->invalidated.empty());

					// extra tree node for every submission
					auto flags = int(ImGuiTreeNodeFlags_FramePadding);
					auto id = dlg::format("Commands:{}", r);
					auto name = "<Unnamed>";
					if(rec->cbName) {
						name = rec->cbName;
					}

					if(ImGui::TreeNodeEx(id.c_str(), flags, "%s", name)) {
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

						DisplayVisitor visitor(selected, commandFlags_, brokenLabelNesting_);
						visitor.displayCommands(rec->commands, true);
						auto nsel = std::move(visitor.newSelection_);

						if(!nsel.empty() && (command_.empty() || nsel.back() != command_.back())) {
							record_ = rec;
							command_ = std::move(nsel);
							selectedBatch_ = records_;
							selectedRecord_ = rec;

							// TODO: fix snapshotRelevantDescriptors for destroyed bindings
							// TODO: do full snapshot in hook and use that here (if we already have a completed hook)?
							//  descriptors might already be destroyed, snapshotRelevantDescriptors won't help
							auto dsSnapshot = CommandDescriptorSnapshot{}; // snapshotRelevantDescriptors(*command_.back());
							commandViewer_.select(record_, *command_.back(),
								dsSnapshot, true, nullptr);

							dev.commandHook->target = {};
							dev.commandHook->target.all = true;
							dev.commandHook->desc(record_, command_, dsSnapshot);
							dev.commandHook->freeze = false;
						}

						ImGui::Indent(s);
						ImGui::TreePop();
					}

					if(r + 1 != batch.submissions.size()) {
						ImGui::Separator();
					}
				}

				ImGui::Indent(s);
				ImGui::TreePop();

				if(b + 1 != records_.size()) {
					ImGui::Separator();
				}
			}
		}
	} else {
		dlg_assert(record_);

		// When the record isn't valid anymore (cb is unset), we have to
		// be careful to not actually reference any destroyed resources.
		if(record_ && !record_->cb) {
			replaceInvalidatedLocked(*record_);
		}

		dlg_assert(record_->invalidated.empty());

		auto* selected = command_.empty() ? nullptr : command_.back();

		if(!brokenLabelNesting_ && record_->brokenHierarchyLabels) {
			brokenLabelNesting_ = true;
			// showing end commands in this viewing mode since
			// they aren't implicitly shown via the nesting
			// now anymore and stuff like EndRenderPass is
			// kinda important.
			commandFlags_ |= Command::Type::end;
		}

		DisplayVisitor visitor(selected, commandFlags_, brokenLabelNesting_);
		visitor.displayCommands(record_->commands, false);
		auto nsel = std::move(visitor.newSelection_);

		if(!nsel.empty() && (command_.empty() || nsel.back() != command_.back())) {
			if(mode_ == UpdateMode::none) {
				dlg_assert(dev.commandHook->target.record == record_.get());
			} else if(mode_ == UpdateMode::commandBuffer) {
				dlg_assert(cb_);
				dlg_assert(dev.commandHook->target.cb == cb_);
			} else if(mode_ == UpdateMode::any) {
				dlg_assert(dev.commandHook->target.all);
			}

			command_ = std::move(nsel);

			// TODO: fix snapshotRelevantDescriptors for destroyed bindings
			// TODO: do full snapshot in hook and use that here (if we already have a completed hook)?
			//  descriptors might already be destroyed, snapshotRelevantDescriptors won't help
			auto dsSnapshot = CommandDescriptorSnapshot{}; // snapshotRelevantDescriptors(*command_.back());
			commandViewer_.select(record_, *command_.back(),
				dsSnapshot, true, nullptr);

			// in any case, update the hook
			dev.commandHook->desc(record_, command_, dsSnapshot);
			dev.commandHook->freeze = false;
		}
	}

	ImGui::PopStyleVar(2);

	updateState();

	ImGui::EndChild();
	// ImGui::PopStyleColor();
	ImGui::TableNextColumn();

	// command info
	ImGui::BeginChild("Command Info");
	commandViewer_.draw(draw);

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
	auto& hook = *gui_->dev().commandHook;
	hook.unsetHookOps();
	hook.desc({}, {}, {});

	command_ = {};
	record_ = std::move(record);
	selectedRecord_ = {};

	updateHookTarget();

	commandViewer_.unselect();
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
	auto& hook = *gui_->dev().commandHook;
	hook.unsetHookOps();
	hook.desc({}, {}, {});

	command_ = {};
	record_ = std::move(record);
	selectedRecord_ = {};

	updateHookTarget();

	commandViewer_.unselect();
}

void CommandBufferGui::showSwapchainSubmissions() {
	mode_ = UpdateMode::swapchain;
	cb_ = {};

	command_ = {};
	record_ = {};
	selectedRecord_ = {};

	// Unset hooks
	auto& hook = *gui_->dev().commandHook;
	hook.unsetHookOps();
	hook.desc({}, {}, {});
	hook.target = {};

	commandViewer_.unselect();
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
			// hook.target.group = nonNull(record_).group;
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

	// try to update the shown commands with the best new match
	if(!hook.completed.empty() && (!freezeState_ || !commandViewer_.state())) {
		// find the best match
		CommandHook::CompletedHook* best = nullptr;
		auto bestMatch = 0.f;
		u32 bestPresentID = {};
		span<const RecordBatch> bestBatches; // only for swapchain mode

		for(auto& res : hook.completed) {
			float resMatch = res.match;
			span<const RecordBatch> foundBatches;
			u32 presentID {};

			// When we are in swapchain mode, we need the frame associated with
			// this submission. We will then also consider the submission in its
			// context inside the frame.
			if(mode_ == UpdateMode::swapchain) {
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

				// check if [selectedRecord_ in selectedBatch_] contextually
				// matches  [res.record in foundBatches]
				dlg_assert(!selectedBatch_.empty());
				auto batchMatches = match(foundBatches, selectedBatch_);
				bool recordMatched = false;
				for(auto& batchMatch : batchMatches.matches) {
					if(batchMatch.a->submissionID != res.submissionID) {
						continue;
					}

					for(auto& recMatch : batchMatch.matches) {
						if(recMatch.a != res.record) {
							continue;
						}

						if(recMatch.b == selectedRecord_.get()) {
							recordMatched = true;
							resMatch *= recMatch.match;
						}

						break;
					}

					break;
				}

				// In this case, the newly hooked potentialy candidate didn't
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
			}
		}

		if(best) {
			// we alwyas update the commandHook desc (even if we don't
			// update the internal state below) since this record is newer
			// than the old one and might have valid handles/state that is
			// already unset in the old one.
			dev.commandHook->desc(best->record, best->command,
				best->descriptorSnapshot, false);

			// update internal state state from hook match
			selectedRecord_ = best->record;

			if(!freezeCommands_) {
				record_ = best->record;
				command_ = best->command;
			}

			// In swapchain mode - and when not freezing commands - make
			// sure to also display the new frame
			if(mode_ == UpdateMode::swapchain) {
				swapchainPresent_ = bestPresentID;
				selectedBatch_ = {bestBatches.begin(), bestBatches.end()};

				if(!freezeCommands_) {
					records_ = {bestBatches.begin(), bestBatches.end()};
				}
			}

			// update command viewer state from hook match
			// TODO: this call messes with the commandHook, potentially
			// invalidated 'best'. And messing with freeze. Should
			// clean up responsibilities here. If we don't have
			// to call this here, we could also move from the vectors
			// from *best above.
			commandViewer_.select(best->record, *best->command.back(),
				best->descriptorSnapshot, false, best->state);

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
	if(!freezeCommands_ && command_.empty()) {
		if(mode_ == UpdateMode::swapchain) {
			records_ = gui_->dev().swapchain->frameSubmissions[0].batches;
			swapchainPresent_ = gui_->dev().swapchain->frameSubmissions[0].presentID;
		} else if(mode_ == UpdateMode::commandBuffer) {
			dlg_assert(cb_);
			record_ = cb_->lastRecordPtrLocked();
		} else if(mode_ == UpdateMode::any) {
			// TODO: correct updating, iterate through all submissions
			// in the last frames and find the best record match. ouch.
			dlg_error("TODO: shown commands not correctly updated until selected");
		}
	}

	hook.completed.clear();
}

} // namespace vil
