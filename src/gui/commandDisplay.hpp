#pragma once

#include <cb.hpp>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <imgui/imgui.h>
#include <gui/util.hpp>
#include <util/util.hpp>

namespace vil {

struct DisplayVisitor : CommandVisitor {
	std::unordered_set<const ParentCommand*>& openedSections_;

	const Command* sel_;
	Command::CategoryFlags flags_;
	std::vector<const Command*> newSelection_;

	// whether to use broken-hierarchy mode
	bool labelOnlyIndent_ {};
	bool open_ {true};
	u32 indent_ {};

	bool jumpToSelection_ {};
	bool forbidNewSelection_ {};
	bool showSingleSections_ {};

	DisplayVisitor(std::unordered_set<const ParentCommand*>& opened,
			const Command* sel, Command::CategoryFlags flags, bool labelOnlyIndent) :
		openedSections_(opened), sel_(sel), flags_(flags), labelOnlyIndent_(labelOnlyIndent) {
	}

	~DisplayVisitor() {
		finish();
	}

	bool display(const RootCommand& cmd, bool firstSep) {
		auto selected = displayCommands(cmd.children_, firstSep);
		if(selected) {
			newSelection_.insert(newSelection_.begin(), &cmd);
		}
		return selected;
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

	// returns
	bool drawNode(const Command& cmd, ImGuiTreeNodeFlags flags) {
		auto disable = forbidNewSelection_ && sel_ != &cmd;
		if(disable) {
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
		}

		const auto open = ImGui::TreeNodeEx(static_cast<const void*>(&cmd),
			flags, "%s", cmd.toString().c_str());

		if(disable) {
			ImGui::PopStyleColor(2u);
		}

		return open;
	}

	// Returns whether one of the children was selected
	bool displayCommands(const Command* cmd, bool firstSep) {
		// TODO PERF: should use imgui list clipper, might have *a lot* of commands here.
		// But first we have to restrict what cmd->display can actually do.
		// Would also have to pre-filter commands for that. And stop at every
		// (expanded) parent command (but it's hard to tell whether they are
		// expanded).
		auto showSep = firstSep;
		auto emptyBefore = newSelection_.empty();
		while(cmd) {

			// No matter the flags, we never want to hide parent commands.
			auto visible = (flags_ & cmd->category()) || (cmd == sel_);
			if(open_ && visible) {
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
			ImGuiTreeNodeFlags_SpanFullWidth |
			ImGuiTreeNodeFlags_OpenOnDoubleClick;
		if(sel_ == &cmd) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		const auto opened = openedSections_.count(&cmd);
		ImGui::SetNextItemOpen(opened);

		const auto open = drawNode(cmd, flags);

		// don't select when only clicked on arrow
		if(ImGui::IsItemActivated() &&
				!forbidNewSelection_ &&
				!ImGui::IsItemToggledOpen()) {
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
		// never skip the selected command
		auto visible = (flags_ & cmd.category()) || (&cmd == sel_);
		if(!visible || !open_) {
			return;
		}

		int flags = ImGuiTreeNodeFlags_Leaf |
			ImGuiTreeNodeFlags_Bullet |
			ImGuiTreeNodeFlags_NoTreePushOnOpen |
			ImGuiTreeNodeFlags_SpanFullWidth |
			ImGuiTreeNodeFlags_FramePadding;
		if(sel_ == &cmd) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		drawNode(cmd, flags);

		auto activated = ImGui::IsItemActivated() && !forbidNewSelection_;
		if(activated) {
			dlg_assert(newSelection_.empty());
			newSelection_.push_back(&cmd);
		}

		if(jumpToSelection_ && (sel_ == &cmd || activated)) {
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
			if(children && !showSingleSections_) {
				// If we only have one subpass, don't give it an extra section
				// to make everything more compact.
				first = deriveCast<FirstSubpassCmd*>(children);
				dlg_assert(first);
				// for single-subpass renderpasses, this should
				// be the EndRenderPassCmd
				dlg_assert(first->next);
				if(commandCast<EndRenderPassCmd*>(first->next)) {
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
		// lables can pass CmdExecuteCommands boundaries, even if it's weird.
		// otherwise we could always indent here as well.
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
			if(children && !showSingleSections_) {
				// If we only have one command buffer, don't give it an extra section
				// to make everything more compact.
				first = deriveCast<ExecuteCommandsChildCmd*>(children);
				dlg_assert(first);
				if(!first->next) {
					children = first->record_->commands->children_;
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

} // namespace vil
