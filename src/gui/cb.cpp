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
#include <util/spirv_reflect.h>
#include <bitset>

namespace vil {

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
	if(ImGui::Button("Visible Commands")) {
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

		ImGui::EndPopup();
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
		if(!gui_->dev().swapchain) {
			record_ = {};
			records_ = {};
			swapchainCounter_ = {};
			dsState_ = {};
			command_ = {};
			hook.target = {};
			hook.desc({}, {}, {});
			hook.unsetHookOps();
			return;
		}

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
				ImGui::Separator();

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
					if(ImGui::TreeNodeEx(id.c_str(), flags, "Commands")) {
						if(rec->commands) {
							ImGui::Separator();
						}

						// we don't want as much space as tree nodes
						auto s = 0.3 * ImGui::GetTreeNodeToLabelSpacing();
						ImGui::Unindent(s);

						auto nsel = displayCommands(rec->commands, selected, commandFlags_);
						if(!nsel.empty() && (command_.empty() || nsel.back() != command_.back())) {
							record_ = rec;
							command_ = std::move(nsel);
							dsState_ = record_->lastDescriptorState;

							commandViewer_.select(record_, *command_.back(), dsState_, true);

							dev.commandHook->target = {};
							dev.commandHook->target.all = true;
							dev.commandHook->desc(record_, command_, dsState_);
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
		auto nsel = displayCommands(record_->commands, selected, commandFlags_);
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
			dsState_ = record_->lastDescriptorState;
			commandViewer_.select(record_, *command_.back(), dsState_, true);

			// in any case, update the hook
			dev.commandHook->desc(record_, command_, dsState_);
			dev.commandHook->freeze = false;
		}
	}

	ImGui::PopStyleVar(2);

	// try to update the shown commands with the best new match
	if(!hook.completed.empty() && (!freezeCommands_ || !commandViewer_.state())) {
		// find the best match
		// TODO: when in swapchain mode, we should match the context, i.e.
		// that the new record appeared in roughly the same position on
		// the queue as our old one. See command group LCS in todo.md
		auto* best = &hook.completed[0];
		if(hook.completed.size() > 1) {
			// dlg_info("multiple matches!");
			for(auto& res : hook.completed) {
				// dlg_info(">> {}", res.match);
				if(res.match > best->match) {
					best = &res;
				}
			}
		}

		dlg_assert(!best->state->writer);

		// update command viewer state from hook match
		commandViewer_.select(best->record, *best->command.back(),
			best->descriptorSnapshot, false);
		commandViewer_.state(best->state);

		// we alwyas update the commandHook desc (even if we don't
		// update the internal state below) since this record is newer
		// than the old one and might have valid handles/state that is
		// already unset in the old one.
		dev.commandHook->desc(best->record, best->command,
			best->descriptorSnapshot, false);

		// update internal state state from hook match
		if(!freezeCommands_) {
			record_ = std::move(best->record);
			command_ = best->command;
			dsState_ = std::move(best->descriptorSnapshot);
		}

		if(mode_ == UpdateMode::swapchain && !freezeCommands_) {
			auto bestSubID = best->submissionID;
			dlg_assert(gui_->dev().swapchain);

			// TODO: handle the case when we don't find it.
			// Can happen if the completed hook is from too long ago, e.g.
			// when just opening gui again/switching to cb tab again.
			auto found = false;
			for(auto& frame : gui_->dev().swapchain->frameSubmissions) {
				if(bestSubID >= frame.submissionStart && bestSubID <= frame.submissionEnd) {
					records_ = frame.batches;
					found = true;
					break;
				}
			}

			dlg_assertl(dlg_level_warn, found);
		}

		if(!hook.freeze && freezeState_) {
			hook.freeze = true;
		}
	}

	// When not command was selected yet, we won't get any new records
	// from CommandHook. But still want to show the new commands.
	if(!freezeCommands_ && command_.empty()) {
		if(mode_ == UpdateMode::swapchain) {
			records_ = gui_->dev().swapchain->frameSubmissions[0].batches;
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

	ImGui::EndChild();
	// ImGui::PopStyleColor();
	ImGui::TableNextColumn();

	// command info
	ImGui::BeginChild("Command Info");
	if(!command_.empty()) {
		commandViewer_.draw(draw);
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
	auto& hook = *gui_->dev().commandHook;
	hook.unsetHookOps();
	hook.desc({}, {}, {});

	command_ = {};
	record_ = std::move(record);
	dsState_ = {};

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
	dsState_ = {};

	updateHookTarget();

	commandViewer_.unselect();
}

void CommandBufferGui::showSwapchainSubmissions() {
	mode_ = UpdateMode::swapchain;
	cb_ = {};

	command_ = {};
	record_ = {};
	dsState_ = {};

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

} // namespace vil
