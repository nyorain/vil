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
#include <spirv_reflect.h>
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
			case UpdateMode::commandGroup: return "CommandGroup";
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

			/*
			if(record_ && record_->group && ImGui::Selectable("CommandGroup")) {
				mode_ = UpdateMode::commandGroup;
				hook.target = {};
				hook.target.group = record_->group;
			}
			*/

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
	ImGui::Checkbox("Freeze State", &hook.freeze);

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
	} else if(mode_ == UpdateMode::commandGroup) {
		dlg_assert(false);
		// dlg_assert(record_->group);
		// imGuiText("Updating from Command Group");
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

		ImGui::SameLine();
		ImGui::Checkbox("Freeze Commands", &freezePresentBatches_);
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

	if(mode_ == UpdateMode::swapchain) {
		if(records_.empty() && !gui_->dev().swapchain->frameSubmissions.empty()) {
			records_ = gui_->dev().swapchain->frameSubmissions[0].batches;
		}

		auto* selected = record_ && !command_.empty() ? command_.back() : nullptr;

		for(auto b = 0u; b < records_.size(); ++b) {
			auto& batch = records_[b];
			auto id = dlg::format("vkQueueSubmit:{}", b);

			auto flags = 0u;
			if(batch.submissions.empty()) {
				flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
			}

			if(ImGui::TreeNodeEx(id.c_str(), flags, "vkQueueSubmit")) {
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
					// TODO: only show this when there is more than one and
					// then show index as well? might mess with ids though.
					auto flags = 0u;
					// auto id = rec->group;
					auto id = dlg::format("Commands:{}", r);
					if(ImGui::TreeNodeEx(id.c_str(), flags, "Commands")) {
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
							// dlg_assert(record_->group);
							// dlg_assert(desc_.size() <= record_->group->desc.children.size() + 1);

							// dev.commandHook->target.group = record_->group;
							dev.commandHook->target.all = true;
							dev.commandHook->desc(record_, command_, dsState_);
						}

						ImGui::Indent(s);
						ImGui::TreePop();
					}
				}

				ImGui::Indent(s);
				ImGui::TreePop();
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
			} else if(mode_ == UpdateMode::commandGroup) {
				dlg_assert(false);
				// dlg_assert(record_->group);
				// dlg_assert(dev.commandHook->target.group == record_->group);
			}

			command_ = std::move(nsel);
			dsState_ = record_->lastDescriptorState;
			commandViewer_.select(record_, *command_.back(), dsState_, true);

			// in any case, update the hook
			dev.commandHook->desc(record_, command_, dsState_);
		}
	}

	if(!hook.completed.empty() && (!hook.freeze || !commandViewer_.state())) {
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
		if(mode_ != UpdateMode::swapchain || !freezePresentBatches_) {
			record_ = std::move(best->record);
			command_ = best->command;
			dsState_ = std::move(best->descriptorSnapshot);
		}

		auto bestSubID = best->submissionID;
		hook.completed.clear();

		if(mode_ == UpdateMode::swapchain && !freezePresentBatches_) {
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
	}

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

void CommandBufferGui::selectGroup(IntrusivePtr<CommandRecord> record) {
	mode_ = UpdateMode::commandGroup;
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
	hook.target = {};

	command_ = {};
	record_ = std::move(record);
	dsState_ = {};

	updateHookTarget();

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
			if(hook.target.cb) {
				dlg_assert(hook.target.cb == &handle);
				hook.target = {};
				hook.target.record = record_.get();
			}
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
		case UpdateMode::commandGroup:
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
