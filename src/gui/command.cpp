#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/command.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/cb.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <util/profiling.hpp>
#include <util/buffmt.hpp>
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <device.hpp>
#include <swapchain.hpp>
#include <cb.hpp>
#include <queue.hpp>
#include <threadContext.hpp>
#include <pipe.hpp>
#include <accelStruct.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <ds.hpp>
#include <shader.hpp>
#include <rp.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <imgui/imgui_internal.h>
#include <bitset>

#ifdef VIL_COMMAND_CALLSTACKS
	#include <backward/resolve.hpp>
#endif // VIL_COMMAND_CALLSTACKS

// NOTE: since we might view invalidated command records, we can't assume
//   any handles in Command objects to be not null (they are unset to null
//   when the cooresponding handle is destroyed/invalidated).

namespace vil {
namespace {

#ifdef VIL_COMMAND_CALLSTACKS
void display(span<void*> st, unsigned offset = 6u) {
	if (st.size() <= offset) {
		imGuiText("No callstack");
		return;
	}

	ThreadMemScope tms;
	auto resolved = backward::resolve(tms, st.subspan(offset));

	for(auto i = 0u; i < resolved.size(); ++i) {
		auto& loc = resolved[i];
		if(loc.function.empty()) {
			loc.function = dlg::format("[{}]", st[i + offset]);
		}
		imGuiText("#{}: {}:{}:{}: {}", i + offset, loc.filename, loc.line,
			loc.col, loc.function);

		// 	// TODO, something like this. But make it configurable.
		// 	And ffs, don't use std::sytem.
		// if(ImGui::IsItemClicked()) {
		// 	auto base = std::filesystem::current_path();
		// 	auto cmd = dlg::format("nvr -c \"e +{} {}/{}\"", loc.line,
		// 		base.string(), loc.filename);
		// 	(void) std::system(cmd.c_str());
		// }
	}
}
#endif // VIL_COMMAND_CALLSTACKS

u32 transferCount(const CopyBufferCmd& cmd) { return cmd.regions.size(); }
u32 transferCount(const CopyImageCmd& cmd) { return cmd.copies.size(); }
u32 transferCount(const CopyBufferToImageCmd& cmd) { return cmd.copies.size(); }
u32 transferCount(const CopyImageToBufferCmd& cmd) { return cmd.copies.size(); }
u32 transferCount(const BlitImageCmd& cmd) { return cmd.blits.size(); }
u32 transferCount(const ClearColorImageCmd& cmd) { return cmd.ranges.size(); }
u32 transferCount(const ClearDepthStencilImageCmd& cmd) { return cmd.ranges.size(); }
u32 transferCount(const ResolveImageCmd& cmd) { return cmd.regions.size(); }
u32 transferCount(const FillBufferCmd&) { return 1u; }
u32 transferCount(const UpdateBufferCmd&) { return 1u; }

BufferInterval srcBufInterval(const CopyBufferCmd& cmd, u32 idx) {
	dlg_assert(idx < cmd.regions.size());
	return {cmd.regions[idx].srcOffset, cmd.regions[idx].size};
}

BufferInterval dstBufInterval(const CopyBufferCmd& cmd, u32 idx) {
	dlg_assert(idx < cmd.regions.size());
	return {cmd.regions[idx].dstOffset, cmd.regions[idx].size};
}

BufferInterval dstBufInterval(const FillBufferCmd& cmd, u32 idx) {
	dlg_assert(idx == 0u);
	return {cmd.offset, cmd.size};
}

BufferInterval dstBufInterval(const UpdateBufferCmd& cmd, u32 idx) {
	dlg_assert(idx == 0u);
	return {cmd.offset, cmd.data.size()};
}

BufferInterval srcBufInterval(const CopyBufferToImageCmd& cmd, u32 idx) {
	auto texelSize = FormatTexelSize(cmd.dst->ci.format);
	return minMaxInterval({{cmd.copies[idx]}}, texelSize);
}

BufferInterval dstBufInterval(const CopyImageToBufferCmd& cmd, u32 idx) {
	auto texelSize = FormatTexelSize(cmd.src->ci.format);
	return minMaxInterval({{cmd.copies[idx]}}, texelSize);
}

} // anon namespace

// CommandViewer
CommandViewer::CommandViewer() = default;
CommandViewer::~CommandViewer() = default;

Device& CommandViewer::dev() const {
	return gui_->dev();
}

void CommandViewer::init(Gui& gui) {
	gui_ = &gui;
	vertexViewer_.init(gui);
	bufferViewer_.init(gui);
	imageViewer_.init(gui);
	shaderDebugger_.init(gui);
}

void CommandViewer::unselect() {
	view_ = IOView::command;
	viewData_.command.selected = 0;
	command_.clear();
	record_.reset();
}

const RenderPassInstanceState* findRPI(span<const Command* const> cmdh) {
	for(auto* cmd : cmdh) {
		auto renderSection = dynamic_cast<const RenderSectionCommand*>(cmd);
		if(renderSection) {
			return &renderSection->rpi;
		}
	}

	return nullptr;
}

void CommandViewer::updateFromSelector(bool forceUpdateHook) {
	dlg_assert(command_.empty() == !record_);

	auto& sel = selection();
	bool isLocalCapture = sel.updateMode() == CommandSelection::UpdateMode::localCapture;
	auto cmdh = sel.command();
	showBeforeCheckbox_ = false;

	const DrawCmdBase* drawCmd {};
	const StateCmdBase* stateCmd {};

	dlg_assert(!cmdh.empty());

	auto& cmd = *cmdh.back();
	switch(cmd.category()) {
		case CommandCategory::draw:
			drawCmd = deriveCast<const DrawCmdBase*>(&cmd);
			stateCmd = drawCmd;
			break;
		case CommandCategory::dispatch:
			stateCmd = deriveCast<const DispatchCmdBase*>(&cmd);
			break;
		case CommandCategory::traceRays:
			stateCmd = deriveCast<const TraceRaysCmdBase*>(&cmd);
			break;
		default:
			break;
	}

	auto selectCommandView = false;

	// update view_, only keep it if it still makes sense
	switch(view_) {
		case IOView::command:
			// will always stay valid
			if(command_.empty()) {
				selectCommandView = true;
			}
			break;
		case IOView::mesh:
			if(!drawCmd) {
				selectCommandView = true;
			}
			break;
		case IOView::attachment: {
			if(!drawCmd) {
				selectCommandView = true;
				break;
			}

			auto [type, id] = viewData_.attachment;

			auto* lastRPI = findRPI(command_);
			auto* newRPI = findRPI(cmdh);
			dlg_assert(lastRPI);
			dlg_assert(newRPI);

			auto& rpiNew = *newRPI;
			auto& rpiOld = *lastRPI;

			// when the number of attachments match, we keep the selection.
			// NOTE: not sure what makes sense here. Also check for unused
			// attachments?
			if(rpiNew.colorAttachments.size() != rpiOld.colorAttachments.size() ||
					rpiNew.inputAttachments.size() != rpiOld.inputAttachments.size() ||
					!!rpiNew.depthStencilAttachment != !!rpiOld.depthStencilAttachment) {
				selectCommandView = true;
				break;
			}

			if(type == AttachmentType::color) {
				dlg_assert(id < rpiNew.colorAttachments.size());
				if(!rpiNew.colorAttachments[id]) {
					selectCommandView = true;
					break;
				}
			} else if(type == AttachmentType::input) {
				dlg_assert(id < rpiNew.inputAttachments.size());
				if(!rpiNew.inputAttachments[id]) {
					selectCommandView = true;
					break;
				}
			} else if(type == AttachmentType::depthStencil) {
				dlg_assert(id == 0u);
				if(!rpiNew.depthStencilAttachment) {
					selectCommandView = true;
					break;
				}
			}

			break;
		} case IOView::ds: {
			if(!stateCmd) {
				selectCommandView = true;
				break;
			}

			dlg_assert(!command_.empty());
			auto* lastStateCmd = deriveCast<const StateCmdBase*>(command_.back());
			dlg_assert(lastStateCmd);

			// when the newly select command uses a descriptor set with different
			// layout at the selected set we reset the selection. We don't
			// compare cmd->state.descriptorSets as that is often invalidated.
			// Using the layout of the bound pipe is more robust.
			PipelineLayout* newPL {};
			if(auto* pipe = stateCmd->boundPipe(); pipe) {
				newPL = pipe->layout.get();
			}

			PipelineLayout* oldPL {};
			if(auto* pipe = lastStateCmd->boundPipe(); pipe) {
				oldPL = pipe->layout.get();
			}

			if (!oldPL || !newPL || !compatibleForSetN(*oldPL, *newPL, viewData_.ds.set)) {
				selectCommandView = true;
			}
			break;
		}
		case IOView::transferSrc:
		case IOView::transferDst: {
			// Only keep transferSrc/Dst selection when the command type
			// is exactly the same
			if(command_.empty()) {
				selectCommandView = true;
				break;
			}

			auto& lastCmd = *command_.back();
			if(typeid(lastCmd) != typeid(cmd)) {
				selectCommandView = true;
				break;
			}

			// always reset this
			viewData_.transfer.index = 0u;
			break;
		} case IOView::pushConstants: {
			if(!stateCmd || !stateCmd->boundPipe()) {
				selectCommandView = true;
				break;
			}

			// we keep the selection if pcr in the selected stage exists in
			// the new command
			auto pipeStages = stages(*stateCmd->boundPipe());
			selectCommandView = true;

			for(auto& stage : pipeStages) {
				if(viewData_.pushConstants.stage != stage.stage) {
					continue;
				}

				// don't need to respect specialization constants.
				auto& mod = *stage.spirv->compiled;
				if(!mod.get_shader_resources().push_constant_buffers.empty()) {
					selectCommandView = false;
				}

				break;
			}

			break;
		} case IOView::shader: {
			dlg_assert(!command_.empty());
			auto* lastStateCmd = dynamic_cast<const StateCmdBase*>(command_.back());
			dlg_assert(lastStateCmd);

			if(!stateCmd || !stateCmd->boundPipe()
					|| stateCmd->boundPipe() != lastStateCmd->boundPipe()) {
				selectCommandView = true;
				shaderDebugger_.unselect();
			} else if(isLocalCapture && sel.completedHookState()) {
				shaderDebugger_.initVarMap();
			}

			break;
		}
	}

	command_ = {cmdh.begin(), cmdh.end()};
	record_ = sel.record(); // make sure it stays alive

	if(selectCommandView) {
		view_ = IOView::command;
		viewData_.command = {};

		if(!isLocalCapture) {
			imageViewer_.reset(true);
		}
	}

	if(!isLocalCapture && (selectCommandView || forceUpdateHook)) {
		updateHook();
	}
}

void CommandViewer::displayTransferIOList() {
	dlg_assert(!command_.empty());
	auto& cmd = *command_.back();

	dlg_assert(cmd.category() == CommandCategory::transfer);

	// TODO: add support for viewing buffers here.
	// Hard to do in a meaningful way though.
	auto found = false;
	auto flags = ImGuiTreeNodeFlags_Bullet |
		ImGuiTreeNodeFlags_Leaf |
		ImGuiTreeNodeFlags_SpanFullWidth |
		ImGuiTreeNodeFlags_NoTreePushOnOpen |
		ImGuiTreeNodeFlags_FramePadding;

	auto addSrc = [&](auto* cmd) {
		if(!cmd) {
			return;
		}

		found = true;

		auto lflags = flags;
		if(view_ == IOView::transferSrc) {
			lflags |= ImGuiTreeNodeFlags_Selected;
		}

		// TODO: display more information about src
		ImGui::TreeNodeEx("Source", lflags);
		if(ImGui::IsItemActivated()) {
			view_ = IOView::transferSrc;
			viewData_.transfer.index = 0u; // always reset
			imageViewer_.reset(true);
			updateHook();
		}
	};

	auto addDst = [&](auto* cmd) {
		if(!cmd) {
			return;
		}

		found = true;

		auto lflags = flags;
		if(view_ == IOView::transferDst) {
			lflags |= ImGuiTreeNodeFlags_Selected;
		}

		// TODO: display more information about dst
		ImGui::TreeNodeEx("Destination", lflags);
		if(ImGui::IsItemActivated()) {
			view_ = IOView::transferDst;
			viewData_.transfer.index = 0u; // always reset
			imageViewer_.reset(true);
			updateHook();
		}
	};

	auto addSrcDst = [&](auto* cmd) {
		if(!cmd) {
			return;
		}
		addSrc(cmd);
		addDst(cmd);
	};

	addSrcDst(commandCast<const CopyImageCmd*>(&cmd));
	addSrcDst(commandCast<const CopyBufferToImageCmd*>(&cmd));
	addSrcDst(commandCast<const CopyImageToBufferCmd*>(&cmd));
	addSrcDst(commandCast<const BlitImageCmd*>(&cmd));
	addSrcDst(commandCast<const ResolveImageCmd*>(&cmd));
	addSrcDst(commandCast<const CopyBufferCmd*>(&cmd));
	addSrcDst(commandCast<const UpdateBufferCmd*>(&cmd));
	addDst(commandCast<const ClearColorImageCmd*>(&cmd));
	addDst(commandCast<const ClearDepthStencilImageCmd*>(&cmd));
	addDst(commandCast<const FillBufferCmd*>(&cmd));

	if(auto* ca = dynamic_cast<const ClearAttachmentCmd*>(&cmd)) {
		found = true;

		for(auto [i, att] : enumerate(ca->attachments)) {
			auto lflags = flags;
			if(view_ == IOView::transferDst && viewData_.transfer.index == i) {
				lflags |= ImGuiTreeNodeFlags_Selected;
			}

			auto label = dlg::format("Attachment {}", att.colorAttachment);
			ImGui::TreeNodeEx(label.c_str(), lflags);
			if(ImGui::IsItemActivated()) {
				view_ = IOView::transferDst;
				viewData_.transfer.index = i;
				imageViewer_.reset(true);
				updateHook();
			}
		}
	}

	dlg_assertlm(dlg_level_warn, found, "IO inspector unimplemented for command");
}

void CommandViewer::displayDsList() {
	dlg_assert(!command_.empty());

	auto& baseCmd = *command_.back();
	const auto& dsState = selection().descriptorSnapshot();

	const StateCmdBase* cmd {};
	const DrawCmdBase* drawCmd {};
	const DispatchCmdBase* dispatchCmd {};
	const TraceRaysCmdBase* traceCmd {};

	switch(baseCmd.category()) {
		case CommandCategory::draw:
			drawCmd = deriveCast<const DrawCmdBase*>(&baseCmd);
			cmd = drawCmd;
			break;
		case CommandCategory::dispatch:
			dispatchCmd = deriveCast<const DispatchCmdBase*>(&baseCmd);
			cmd = dispatchCmd;
			break;
		case CommandCategory::traceRays:
			traceCmd = deriveCast<const TraceRaysCmdBase*>(&baseCmd);
			cmd = traceCmd;
			break;
		default:
			return;
	}

	auto dss = cmd->boundDescriptors().descriptorSets;

	if(!cmd->boundPipe()) {
		ImGui::Text("Error: No pipeline");
		return;
	}

	auto& pipe = *cmd->boundPipe();

	// TODO: make runtime setting?
	// whether to notify of unbound descriptor sets
	static constexpr auto showUnboundSets = true;

	ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	auto toplevelFlags = ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_SpanFullWidth;
	if(ImGui::TreeNodeEx("Descriptors", toplevelFlags)) {
		// NOTE: better to iterate over sets/bindings in shader stages?
		auto size = std::min(dss.size(), cmd->boundPipe()->layout->descriptors.size());
		for(auto setID = 0u; setID < size; ++setID) {
			auto& ds = dss[setID];

			// No descriptor set bound
			if(!ds.dsEntry) {
				if(showUnboundSets) {
					auto label = dlg::format("Descriptor Set {}: unbound", setID);
					auto flags = ImGuiTreeNodeFlags_Bullet |
						ImGuiTreeNodeFlags_Leaf |
						ImGuiTreeNodeFlags_NoTreePushOnOpen |
						ImGuiTreeNodeFlags_SpanFullWidth |
						ImGuiTreeNodeFlags_FramePadding;
					ImGui::TreeNodeEx(label.c_str(), flags);

					if(ImGui::IsItemHovered()) {
						ImGui::BeginTooltip();
						imGuiText("No descriptor was bound for this slot");
						// TODO: check whether the pipeline uses any bindings
						// of this descriptor statically. Warn, if so.
						// Are there any descriptor flags that would allow this?
						ImGui::EndTooltip();
					}
				}

				continue;
			}

			// TODO: this can happen now with descriptor cows
			auto stateIt = dsState.states.find(ds.dsEntry);
			dlg_assert_or(stateIt != dsState.states.end(), continue);

			auto& dsCow = *stateIt->second;

			auto label = dlg::format("Descriptor Set {}", setID);
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			if(ImGui::TreeNodeEx(label.c_str(), toplevelFlags)) {
				auto [state, lock] = access(dsCow);
				for(auto bID = 0u; bID < state.layout->bindings.size(); ++bID) {
					auto sstages = stages(pipe);
					dlg_assert(!sstages.empty());

					std::vector<std::string> stageNames;
					stageNames.resize(sstages.size());

					std::optional<u32> firstValid;
					for(auto i = 0u; i < sstages.size(); ++i) {
						auto& stage = sstages[i];
						// don't need to respect specialization constants.
						auto& mod = *stage.spirv->compiled;
						auto name = bindingName(mod, setID, bID);
						if(name.type == BindingNameRes::Type::valid) {
							stageNames[i] = std::move(name.name);
							firstValid = firstValid.value_or(i);
						} else if(name.type == BindingNameRes::Type::unnamed) {
							stageNames[i] = "<unnamed>";
							firstValid = firstValid.value_or(i);
						} else if(name.type == BindingNameRes::Type::notfound) {
							stageNames[i] = "<not used in this stage>";
						}
					}

					if(!firstValid && !showUnusedBindings_) {
						continue;
					}

					auto flags = ImGuiTreeNodeFlags_Bullet |
						ImGuiTreeNodeFlags_Leaf |
						ImGuiTreeNodeFlags_NoTreePushOnOpen |
						ImGuiTreeNodeFlags_SpanFullWidth |
						ImGuiTreeNodeFlags_FramePadding;
					if(view_ == IOView::ds && viewData_.ds.set == setID && viewData_.ds.binding == bID) {
						flags |= ImGuiTreeNodeFlags_Selected;
					}

					// NOTE
					// - Previews would be the best on the long run but hard to get
					//   right I guess (also: preview of buffers?)
					// - could show name of bound resource(s)?
					// - could show additional information, e.g. shader decorations
					auto id = firstValid ? *firstValid : 0u;
					auto* label = stageNames[id].c_str();
					auto msg = dlg::format("{}: {}", bID, label);

					ImGui::TreeNodeEx(msg.c_str(), flags);
					if(ImGui::IsItemActivated()) {
						view_ = IOView::ds;
						viewData_.ds = {setID, bID, 0, VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM};
						imageViewer_.reset(true);
						doUpdateHook_ = true;
					}
					if(ImGui::IsItemHovered()) {
						ImGui::BeginTooltip();

						auto& layout = state.layout->bindings[bID];
						std::string type = vk::name(layout.descriptorType);
						if(layout.descriptorCount > 1) {
							auto count = descriptorCount(state, bID);
							type += dlg::format("[{}]", count);
							if(layout.flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
								type += " (variable count)";

							}
						}

						imGuiText("Type: {}", type);

						if(sstages.size() > 1u) {
							ImGui::Separator();
							for(auto i = 0u; i < sstages.size(); ++i) {
								imGuiText("{}: {}", vk::name(sstages[i].stage),
									stageNames[i]);
							}
						}

						ImGui::EndTooltip();
					}
				}

				ImGui::TreePop();
			}
		}

		ImGui::TreePop();
	}
}

void CommandViewer::displayIOList() {
	dlg_assert(!command_.empty());
	auto& cmd = *command_.back();

	const StateCmdBase* stateCmd {};
	const DrawCmdBase* drawCmd {};
	const DispatchCmdBase* dispatchCmd {};
	const TraceRaysCmdBase* traceCmd {};

	switch(cmd.category()) {
		case CommandCategory::draw:
			drawCmd = deriveCast<const DrawCmdBase*>(&cmd);
			stateCmd = drawCmd;
			break;
		case CommandCategory::dispatch:
			dispatchCmd = deriveCast<const DispatchCmdBase*>(&cmd);
			stateCmd = dispatchCmd;
			break;
		case CommandCategory::traceRays:
			traceCmd = deriveCast<const TraceRaysCmdBase*>(&cmd);
			stateCmd = traceCmd;
			break;
		default:
			break;
	}

	(void) traceCmd;

	auto flags = ImGuiTreeNodeFlags_Leaf |
		ImGuiTreeNodeFlags_Bullet |
		ImGuiTreeNodeFlags_FramePadding |
		ImGuiTreeNodeFlags_SpanFullWidth |
		ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if(view_ == IOView::command) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::TreeNodeEx("Command", flags);
	if(ImGui::IsItemActivated()) {
		view_ = IOView::command;
		updateHook();
	}

	// Transfer IO
	if(cmd.category() == CommandCategory::transfer) {
		displayTransferIOList();
		return;
	}

	// Vertex IO
	if(drawCmd) {
		auto flags = ImGuiTreeNodeFlags_Leaf |
			ImGuiTreeNodeFlags_Bullet |
			ImGuiTreeNodeFlags_FramePadding |
			ImGuiTreeNodeFlags_SpanFullWidth |
			ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if(view_ == IOView::mesh) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		ImGui::TreeNodeEx("Vertex input", flags);
		if(ImGui::IsItemActivated()) {
			view_ = IOView::mesh;
			viewData_.mesh = {true};
			updateHook();
		}
	}

	// Descriptors
	displayDsList();

	// Attachments
	if(drawCmd) {
		ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
		auto toplevelFlags = ImGuiTreeNodeFlags_FramePadding |
			ImGuiTreeNodeFlags_SpanFullWidth;
		if(ImGui::TreeNodeEx("Attachments", toplevelFlags)) {
			auto* pRPI = findRPI(command_);

			if(pRPI) {
				auto& rpi = *pRPI;
				auto addAttachment = [&](const std::string& label, AttachmentType type,
						unsigned id, ImageView* view) {
					if(!view) {
						return;
					}

					auto flags = ImGuiTreeNodeFlags_Bullet |
						ImGuiTreeNodeFlags_SpanFullWidth |
						ImGuiTreeNodeFlags_Leaf |
						ImGuiTreeNodeFlags_NoTreePushOnOpen |
						ImGuiTreeNodeFlags_FramePadding;
					if(view_ == IOView::attachment &&
							viewData_.attachment.type == type &&
							viewData_.attachment.id == id) {
						flags |= ImGuiTreeNodeFlags_Selected;
					}

					ImGui::TreeNodeEx(label.c_str(), flags);
					if(ImGui::IsItemActivated()) {
						view_ = IOView::attachment;
						viewData_.attachment = {type, id};
						imageViewer_.reset(true);
						updateHook();
					}
				};

				// IDEA: name them if possible? Could use names from (fragment)
				// shader. Or names of image/imageView?
				for(auto [c, att] : enumerate(rpi.colorAttachments)) {
					auto label = dlg::format("Color Attachment {}", c);
					addAttachment(label, AttachmentType::color, c, att);
				}

				for(auto [c, att] : enumerate(rpi.inputAttachments)) {
					auto label = dlg::format("Input Attachment {}", c);
					addAttachment(label, AttachmentType::input, c, att);
				}

				// depth-stencil
				if(rpi.depthStencilAttachment) {
					auto label = dlg::format("Depth Stencil");
					addAttachment(label, AttachmentType::depthStencil,
						0u, rpi.depthStencilAttachment);
				}

				// NOTE: display preserve attachments? resolve attachments?
			} else {
				// TODO: I guess this can happen for secondary command buffers
				// Should display more meaningful error message.
				// And think about how to handle them, anyways...
				// Do we allow selecting a secondary record on its own here?!
				dlg_error("no render pass instance?!");
				imGuiText("Unexpected error: No render pass instance?");
			}

			ImGui::TreePop();
		}
	}

	// display push constants
	auto viewPCRStage = VK_SHADER_STAGE_ALL; // invalid
	if(view_ == IOView::pushConstants) {
		viewPCRStage = viewData_.pushConstants.stage;
	}

	if(stateCmd && stateCmd->boundPipe()) {
		// TODO: push constants for ray tracing can get messy due to
		// high number of shaders, need new concept for visualizing them.

		auto sstages = stages(*stateCmd->boundPipe());
		for(auto& stage : sstages) {
			// specialization constants not relevant here
			auto& compiled = *stage.spirv->compiled;
			if(compiled.get_shader_resources().push_constant_buffers.empty()) {
				continue;
			}

			auto flags = ImGuiTreeNodeFlags_Bullet |
				ImGuiTreeNodeFlags_Leaf |
				ImGuiTreeNodeFlags_SpanFullWidth |
				ImGuiTreeNodeFlags_NoTreePushOnOpen |
				ImGuiTreeNodeFlags_FramePadding;
			if(viewPCRStage == stage.stage) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			auto stageName = vk::name(stage.stage);
			auto label = sstages.size() == 1u ?
				"Push Constants" : dlg::format("Push Constants {}", stageName);
			ImGui::TreeNodeEx(label.c_str(), flags);
			if(ImGui::IsItemActivated()) {
				view_ = IOView::pushConstants;
				viewData_.pushConstants = {stage.stage};
				updateHook();
			}
		}
	}
}

bool CommandViewer::displayBeforeCheckbox() {
	if(ImGui::Checkbox("Before Command", &beforeCommand_)) {
		doUpdateHook_ = true;
		return true;
	}

	return false;
}

void CommandViewer::displayDs(Draw& draw) {
	auto& gui = *this->gui_;

	dlg_assert_or(!command_.empty(), return);
	auto* cmd = deriveCast<const StateCmdBase*>(command_.back());
	dlg_assert_or(cmd, return);
	dlg_assert_or(cmd->boundPipe(), return);

	auto dss = cmd->boundDescriptors().descriptorSets;
	auto& pipe = *cmd->boundPipe();

	dlg_assert(view_ == IOView::ds);
	auto [setID, bindingID, _1, _2] = viewData_.ds;

	if(setID >= dss.size()) {
		ImGui::Text("DescriptorSet out of range");
		dlg_warn("DescriptorSet out of range? Shouldn't happen");
		return;
	}

	auto* setEntry = dss[setID].dsEntry;
	if(!setEntry) {
		ImGui::Text("DescriptorSet null");
		dlg_warn("DescriptorSet null? Shouldn't happen");
		return;
	}

	const auto& descriptors = selection().descriptorSnapshot();
	auto stateIt = descriptors.states.find(setEntry);
	dlg_assert_or(stateIt != descriptors.states.end(), return);

	// NOTE: while holding this lock we MUST not lock the device or
	// queue mutex.
	auto& dsCow = *stateIt->second;
	auto [dsState, lock] = access(dsCow);

	if(bindingID >= dsState.layout->bindings.size()) {
		ImGui::Text("Binding not bound");
		dlg_warn("Binding not bound? Shouldn't happen");
		return;
	}

	auto& bindingLayout = dsState.layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	auto dsCat = category(dsType);

	// NOTE: the "Before Command" checkbox might not make sense for other types.
	// For uniform buffers etc. the contents *should* stay the same but it
	// might still be nice to select when they are captured, for some
	// debugging corner cases. For sampler, otoh, we just don't capture
	// anything anyways.
	if(dsType != VK_DESCRIPTOR_TYPE_SAMPLER) {
		// if(displayBeforeCheckbox()) {
		// 	ImGui::Text("Updating...");
		// 	return;
		// }
		showBeforeCheckbox_ = true;
	}

	auto& elemID = viewData_.ds.elem;
	if(dsCat != DescriptorCategory::inlineUniformBlock) {
		auto bindingCount = descriptorCount(dsState, bindingID);
		if(optSliderRange("Element", elemID, bindingCount)) {
			doUpdateHook_ = true;
		}

		if(elemID >= bindingCount) {
			ImGui::Text("Element not bound");
			dlg_warn("Element not bound? Shouldn't happen");
			return;
		}
	}

	// TODO: we expect descriptors to be valid here. Needs rework
	// for descriptor indexing

	// Find the descriptor in question
	// NOTE: we can't rely on 'state_->copiedDescriptors.size() == 1u' anymore
	//   for the descriptor types that need copies since we want to support
	//   local captures (that might have more data)
	const CommandHookState::CopiedDescriptor* copiedData {};
	auto hookState = selection().completedHookState();
	if(hookState) {
		copiedData = findDsCopy(*hookState, setID, bindingID, elemID,
			beforeCommand_, false);
	}

	// == Buffer ==
	if(dsCat == DescriptorCategory::buffer) {
		imGuiText("{}", vk::name(dsType));

		std::optional<u32> dynOffset;
		if(needsDynamicOffset(dsType)) {
			auto baseOff = bindingLayout.dynOffset;
			dlg_assert(baseOff + elemID < dss[setID].dynamicOffsets.size());
			dynOffset = dss[setID].dynamicOffsets[baseOff + elemID];
		}

		// general info
		auto& elem = buffers(dsState, bindingID)[elemID];
		dlg_assert(elem.buffer);

		refButton(gui, *elem.buffer);
		ImGui::SameLine();
		drawOffsetSize(elem, dynOffset);

		// interpret content
		if(!hookState) {
			ImGui::Text("Waiting for a submission...");
			return;
		}

		if(!copiedData) {
			dlg_error("couldn't find copied descriptor data");
			ImGui::Text("Error copying descriptor. See log output");
			return;
		}

		auto* buf = std::get_if<OwnBuffer>(&copiedData->data);
		if(!buf) {
			dlg_assert(copiedData->data.index() == 0);
			imGuiText("Error copying descriptor buffer. See log output");
			return;
		}

		auto* stage = displayDescriptorStageSelector(pipe, setID, bindingID, dsType);
		if(stage) {
			auto& compiled = specializeSpirv(*stage);
			// TODO: reuse found resource from displayDescriptorStageSelector
			auto res = resource(compiled, setID, bindingID, dsType);
			dlg_assert(res);

			auto name = compiled.get_name(res->id);
			if(name.empty()) {
				name = compiled.get_name(res->base_type_id);
				if(name.empty()) {
					name = "?";
				}
			}

			auto typeID = res->base_type_id;

			ThreadMemScope memScope;
			auto* type = buildType(compiled, typeID, memScope.customUse());
			displayTable(name.c_str(), *type, buf->data());
		} else {
			ImGui::Text("Binding not used in pipeline");
		}
	} else if(dsCat == DescriptorCategory::image) {
		// imGuiText("{}", vk::name(dsType));

		// == Sampler ==
		if(needsSampler(dsType)) {
			if(bindingLayout.immutableSamplers) {
				refButtonExpect(gui, bindingLayout.immutableSamplers[elemID].get());
			} else {
				auto& elem = images(dsState, bindingID)[elemID];
				refButtonExpect(gui, elem.sampler);
			}
		}

		// == Image ==
		if(needsImageView(dsType)) {
			// general info
			auto& elem = images(dsState, bindingID)[elemID];
			dlg_assert(elem.imageView);

			auto& imgView = *elem.imageView;
			// refButton(gui, imgView);

			// ImGui::SameLine();
			// refButtonD(gui, imgView.img);

			// imgView.img can be null if the image was destroyed. This isn't
			// too unlikely since we might have kept the imgView alive since it's
			// still referenced in our DescriptorState
			if(imgView.img) {
				// imGuiText("Format: {}", vk::name(imgView.ci.format));
				// auto& extent = imgView.img->ci.extent;
				// imGuiText("Extent: {}x{}x{}", extent.width, extent.height, extent.depth);

				if(needsImageLayout(dsType)) {
					// imGuiText("Layout: {}", vk::name(elem.layout));
				}

				// content
				if(!hookState) {
					ImGui::Text("Waiting for a submission...");
					return;
				}

				if(!copiedData) {
					dlg_error("couldn't find copied descriptor data");
					ImGui::Text("Error copying descriptor. See log output");
					return;
				}

				auto* img = std::get_if<CopiedImage>(&copiedData->data);
				if(!img) {
					dlg_assert(copiedData->data.index() == 0);
					imGuiText("Error copying descriptor image. See log output");
					return;
				}

				// TODO: hacky, done because displayImage used to
				// acquire the device mutex in some cases
				lock = {};
				dsState = {};

				displayImage(draw, *img);
			}
		}
	} else if(dsCat == DescriptorCategory::bufferView) {
		imGuiText("TODO: bufferView viewer not implemented yet");
	} else if(dsCat == DescriptorCategory::accelStruct) {
		auto& elem = accelStructs(dsState, bindingID)[elemID];
		refButtonExpect(gui, elem.accelStruct);

		// content
		if(!hookState) {
			ImGui::Text("Waiting for a submission...");
			return;
		}

		if(!copiedData) {
			dlg_error("couldn't find copied descriptor data");
			ImGui::Text("Error copying descriptor. See log output");
			return;
		}

		using CapturedAccelStruct = CommandHookState::CapturedAccelStruct;
		auto* capture = std::get_if<CapturedAccelStruct>(&copiedData->data);
		if(!capture || !capture->tlas) {
			imGuiText("Error capturing TLAS. See log output");
			return;
		}

		dlg_assert(capture->tlas->built);
		auto resolveBlas = [&](u64 address) -> AccelStructStatePtr {
			auto it = capture->blases.find(address);
			if(it == capture->blases.end()) {
				dlg_error("Invalid blas address {}", address);
				return {};
			}
			return it->second;
		};

		auto& instances = std::get<AccelInstances>(capture->tlas->data);

		// TODO: also display instance information

		vertexViewer_.displayInstances(draw, instances, gui_->dt(), resolveBlas);
	} else if(dsCat == DescriptorCategory::inlineUniformBlock) {
		auto blockData = inlineUniformBlock(dsState, bindingID);
		imGuiText("Inline Uniform Block, Size {}", blockData.size());

		auto* stage = displayDescriptorStageSelector(pipe, setID, bindingID, dsType);
		if(stage) {
			auto& compiled = specializeSpirv(*stage);
			// TODO: reuse found resource from displayDescriptorStageSelector
			auto res = resource(compiled, setID, bindingID, dsType);
			dlg_assert(res);

			auto name = compiled.get_name(res->id);
			if(name.empty()) {
				name = compiled.get_name(res->base_type_id);
				if(name.empty()) {
					name = "?";
				}
			}

			ThreadMemScope memScope;
			auto* type = buildType(compiled, res->type_id, memScope.customUse());
			displayTable(name.c_str(), *type, blockData);
		} else {
			ImGui::Text("Binding not used in pipeline");
		}
	} else {
		imGuiText("Unsupported descriptoer type {}", vk::name(dsType));
	}
}

void CommandViewer::displayAttachment(Draw& draw) {
	dlg_assert_or(!command_.empty(), return);
	dlg_assert_or(command_.back()->category() == CommandCategory::draw, return);
	// auto* drawCmd = deriveCast<const DrawCmdBase*>(command_.back());

	// NOTE: maybe only show this button for output attachments (color, depthStencil)?
	// Does not make sense otherwise as it stays the same i guess.
	// But could be useful for debugging nonetheless
	// if(displayBeforeCheckbox()) {
	// 	ImGui::Text("Updating...");
	// 	return;
	// }
	showBeforeCheckbox_ = true;

	auto pRPI = findRPI(command_);
	dlg_assert(pRPI);
	auto& rpi = *pRPI;

	// information
	auto [atype, aid] = viewData_.attachment;
	span<ImageView* const> attachments;

	switch(atype) {
		case AttachmentType::color: attachments = rpi.colorAttachments; break;
		case AttachmentType::input: attachments = rpi.inputAttachments; break;
		case AttachmentType::depthStencil:
			attachments = {&rpi.depthStencilAttachment, 1u};
			break;
	}

	dlg_assert(aid < attachments.size());

	// refButtonD(*gui_, attachments[aid]);
	if(attachments[aid]) {
		// refButtonD(*gui_, attachments[aid]->img);
	}

	auto hookState = selection().completedHookState();
	if(hookState) {
		if(hookState->copiedAttachments.empty()) {
			dlg_error("copiedAttachments should not be empty");
			ImGui::Text("No attachment copy found. See log output");
			return;
		}

		auto* attCopy = findAttachmentCopy(*hookState,
			viewData_.attachment.type, viewData_.attachment.id, beforeCommand_);
		if(!attCopy) {
			imGuiText("Error copying attachment. See log output");
			return;
		}

		displayImage(draw, attCopy->data);
	} else {
		ImGui::Text("Waiting for a submission...");
	}
}

void CommandViewer::displayPushConstants() {
	dlg_assert_or(!command_.empty(), return);
	auto* cmd = deriveCast<const StateCmdBase*>(command_.back());
	dlg_assert_or(cmd, return);
	dlg_assert_or(cmd->boundPipe(), return);

	auto viewStage = viewData_.pushConstants.stage;
	auto found = false;

	for(auto& stage : stages(*cmd->boundPipe())) {
		if(stage.stage != viewStage) {
			continue;
		}

		found = true;
		auto& compiled = specializeSpirv(stage);
		auto resources = compiled.get_shader_resources();
		if(resources.push_constant_buffers.empty()) {
			ImGui::Text("Error: No push constants in stage");
			return;
		}

		dlg_assert(resources.push_constant_buffers.size() == 1u);
		auto& pcr = resources.push_constant_buffers[0];
		auto name = compiled.get_name(pcr.id);
		if(name.empty()) {
			name = compiled.get_name(pcr.base_type_id);
			if(name.empty()) {
				name = "?";
			}
		}

		ThreadMemScope memScope;
		auto* type = buildType(compiled, pcr.type_id, memScope.customUse());
		displayTable(name.c_str(), *type, cmd->boundPushConstants().data);
		break;
	}

	if(!found) {
		dlg_warn("Invalid push constants stage? Should not happen! (graphics)");
		ImGui::Text("Error: Invalid push constants stage selected");
		return;
	}
}

void CommandViewer::displayTransferData(Draw& draw) {
	dlg_assert(!command_.empty());
	auto& cmd = *command_.back();

	auto hookState = selection().completedHookState();
	if(!hookState) {
		ImGui::Text("Waiting for a submission...");
		return;
	}

	// NOTE: only show where it makes sense?
	// shouldn't be here for src resources i guess.
	// But could be useful for debugging anyways
	// if(displayBeforeCheckbox()) {
	// 	ImGui::Text("Updating...");
	// 	return;
	// }
	showBeforeCheckbox_ = true;

	dlg_assert(view_ == IOView::transferSrc || view_ == IOView::transferDst);
	OwnBuffer* refBuffer = nullptr;
	CopiedImage* refImage = nullptr;
	u32 tcount = 0u;

	auto refDst = [&](auto* ccmd) {
		if(!ccmd) {
			return;
		}

		tcount = transferCount(*ccmd);
		dlg_assert(view_ == IOView::transferDst);
		refButtonD(*gui_, ccmd->dst);

		if constexpr(std::is_convertible_v<decltype(ccmd->dst), const Buffer*>) {
			auto [offset, size] = dstBufInterval(*ccmd, viewData_.transfer.index);
			ImGui::SameLine();
			drawOffsetSize({ccmd->dst, offset, size});
			refBuffer = beforeCommand_ ?
				&hookState->transferDstBefore.buf :
				&hookState->transferDstAfter.buf;
		} else {
			static_assert(std::is_convertible_v<decltype(ccmd->dst), const Image*>);
			refImage = beforeCommand_ ?
				&hookState->transferDstBefore.img :
				&hookState->transferDstAfter.img;
		}
	};

	auto refSrc = [&](auto* ccmd) {
		if(!ccmd) {
			return;
		}

		tcount = transferCount(*ccmd);
		dlg_assert(view_ == IOView::transferSrc);
		refButtonD(*gui_, ccmd->src);

		if constexpr(std::is_convertible_v<decltype(ccmd->src), const Buffer*>) {
			auto [offset, size] = srcBufInterval(*ccmd, viewData_.transfer.index);
			ImGui::SameLine();
			drawOffsetSize({ccmd->src, offset, size});
			refBuffer = beforeCommand_ ?
				&hookState->transferSrcBefore.buf :
				&hookState->transferSrcAfter.buf;
		} else {
			static_assert(std::is_convertible_v<decltype(ccmd->src), const Image*>);
			refImage = beforeCommand_ ?
				&hookState->transferSrcBefore.img :
				&hookState->transferSrcAfter.img;
		}
	};

	auto refSrcDst = [&](auto* ccmd) {
		if(!ccmd) {
			return;
		}

		tcount = transferCount(*ccmd);
		if(view_ == IOView::transferSrc) {
			refSrc(ccmd);
		} else  {
			refDst(ccmd);
		}
	};

	(void) refSrc;

	refSrcDst(commandCast<const CopyImageCmd*>(&cmd));
	refSrcDst(commandCast<const CopyBufferToImageCmd*>(&cmd));
	refSrcDst(commandCast<const CopyImageToBufferCmd*>(&cmd));
	refSrcDst(commandCast<const BlitImageCmd*>(&cmd));
	refSrcDst(commandCast<const ResolveImageCmd*>(&cmd));

	refSrcDst(commandCast<const CopyBufferCmd*>(&cmd));
	refDst(commandCast<const FillBufferCmd*>(&cmd));
	refDst(commandCast<const ClearColorImageCmd*>(&cmd));
	refDst(commandCast<const ClearDepthStencilImageCmd*>(&cmd));

	if(auto* ccmd = commandCast<const UpdateBufferCmd*>(&cmd); ccmd) {
		if(view_ == IOView::transferSrc) {
			ImGui::Separator();
			imGuiText("Static data of size {}", ccmd->data.size());
			bufferViewer_.display(ccmd->data);
			return;
		} else  {
			refDst(ccmd);
		}
	}

	dlg_assert(bool(refBuffer) ^ bool(refImage));
	dlg_assert(tcount > 0u);

	if(optSliderRange("Transfer", viewData_.transfer.index, tcount)) {
		updateHook();
		imGuiText("Updating...");
		return;
	}

	if(refBuffer && refBuffer->buf) {
		bufferViewer_.display(refBuffer->data());
	} else if(refImage && refImage->image) {
		displayImage(draw, *refImage);
	} else {
		imGuiText("Error copying data. See log output");
	}
}

void CommandViewer::displayVertexViewer(Draw& draw) {
	dlg_assert(!command_.empty());

	auto& cmd = *command_.back();

	dlg_assert_or(cmd.category() == CommandCategory::draw, return);
	auto* drawCmd = deriveCast<const DrawCmdBase*>(&cmd);

	dlg_assert_or(drawCmd, return);
	dlg_assert_or(drawCmd->state->pipe, return);

	auto hookState = selection().completedHookState();
	if(!hookState) {
		ImGui::Text("Waiting for a submission...");
		return;
	}

	// 1: table
	if(ImGui::BeginTabBar("Stage")) {
		if(ImGui::BeginTabItem("Vertex input")) {
			if(viewData_.mesh.output) {
				viewData_.mesh.output = false;
				updateHook();
			} else {
				vertexViewer_.displayInput(draw, *drawCmd, *hookState, gui_->dt());
			}

			ImGui::EndTabItem();
		}

		if(ImGui::BeginTabItem("Vertex Output")) {
			if(!viewData_.mesh.output) {
				viewData_.mesh.output = true;
				updateHook();
			} else {
				vertexViewer_.displayOutput(draw, *drawCmd, *hookState, gui_->dt());
			}

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void CommandViewer::displaySelectedIO(Draw& draw) {
	switch(view_) {
		case IOView::command:
			displayCommand();
			break;
		case IOView::ds:
			displayDs(draw);
			break;
		case IOView::mesh:
			displayVertexViewer(draw);
			break;
		case IOView::attachment:
			displayAttachment(draw);
			break;
		case IOView::pushConstants:
			displayPushConstants();
			break;
		case IOView::transferSrc:
		case IOView::transferDst:
			displayTransferData(draw);
			break;
		case IOView::shader:
			shaderDebugger_.draw();
			break;
	}
}

void CommandViewer::displayActionInspector(Draw& draw, bool skipList) {
	// TODO: don't even display the inspector when we are viewing a static
	// record and that record is invalidated.
	// (or other cases where we know it will never be submitted again)
	// auto& hook = *gui.dev().commandHook;
	// if(hook ... invalid?)
	// 	return true;
	// }

	ZoneScoped;

	if(skipList) {
		ImGui::BeginChild("Command IO Inspector");
		displaySelectedIO(draw);
		ImGui::EndChild();
	} else {
		auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
		if(!ImGui::BeginTable("IO inspector", 2, flags, ImGui::GetContentRegionAvail())) {
			return;
		}

		ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, gui_->uiScale() * 200.f);
		ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();

		ImGui::BeginChild("Command IO list");

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, gui_->uiScale() * ImVec2(4.f, 2.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, gui_->uiScale() * ImVec2(4.f, 4.f));

		displayIOList();

		if(doUpdateHook_) {
			updateHook();
			doUpdateHook_ = false;
		}

		ImGui::PopStyleVar(2);

		ImGui::EndChild();
		ImGui::TableNextColumn();
		ImGui::BeginChild("Command IO Inspector");

		displaySelectedIO(draw);

		ImGui::EndChild();
		ImGui::EndTable();
	}

	if(doUpdateHook_) {
		updateHook();
		doUpdateHook_ = false;
	}
}

void CommandViewer::displayCommand() {
	dlg_assert(!command_.empty());
	dlg_assert(view_ == IOView::command);

	auto hookState = selection().completedHookState();
	if(hookState) {
		auto record = selection().record();
		dlg_assert(record);

		auto lastTime = hookState->neededTime;
		auto validBits = gui_->dev().queueFamilies[record->queueFamily].props.timestampValidBits;
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
		// To avoid UI flickering
		imGuiText("Time: pending");
	}

	auto displayMultidraw = [&](u32 count, bool indexed, ReadBuf cmds) {
		auto& sel = viewData_.command.selected;

		if(count == 0u) {
			imGuiText("No commands (drawCount = 0)");
			sel = 0u;
			return;
		} else if(count == 1u) {
			sel = 0u;
		} else {
			auto lbl = dlg::format("Commands: {}", count);
			optSliderRange(lbl.c_str(), sel, count);
		}

		if(indexed) {
			dlg_assert(cmds.size() == sizeof(VkDrawIndexedIndirectCommand) * count);
			skip(cmds, sel * sizeof(VkDrawIndexedIndirectCommand));

			auto ecmd = read<VkDrawIndexedIndirectCommand>(cmds);
			imGuiText("firstIndex: {}", ecmd.firstIndex);
			imGuiText("indexCount: {}", ecmd.indexCount);
			imGuiText("vertexOffset: {}", ecmd.vertexOffset);
			imGuiText("firstInstance: {}", ecmd.firstInstance);
			imGuiText("instanceCount: {}", ecmd.instanceCount);
		} else {
			dlg_assert(cmds.size() == sizeof(VkDrawIndirectCommand) * count);
			skip(cmds, sel * sizeof(VkDrawIndirectCommand));

			auto ecmd = read<VkDrawIndirectCommand>(cmds);
			imGuiText("firstVertex: {}", ecmd.firstVertex);
			imGuiText("vertexCount: {}", ecmd.vertexCount);
			imGuiText("firstInstance: {}", ecmd.firstInstance);
			imGuiText("instanceCount: {}", ecmd.instanceCount);
		}
	};

	if(hookState && hookState->indirectCopy.size) {
		auto& ic = hookState->indirectCopy;
		auto span = ic.data();
		if(auto* dcmd = commandCast<const DrawIndirectCmd*>(command_.back()); dcmd) {
			displayMultidraw(dcmd->drawCount, dcmd->indexed, span);
		} else if(commandCast<const DispatchIndirectCmd*>(command_.back())) {
			auto ecmd = read<VkDispatchIndirectCommand>(span);
			imGuiText("groups X: {}", ecmd.x);
			imGuiText("groups Y: {}", ecmd.y);
			imGuiText("groups Z: {}", ecmd.z);
		} else if(commandCast<const TraceRaysIndirectCmd*>(command_.back())) {
			auto ecmd = read<VkTraceRaysIndirectCommandKHR>(span);
			imGuiText("width: {}", ecmd.width);
			imGuiText("height: {}", ecmd.height);
			imGuiText("depth: {}", ecmd.depth);
		} else if(auto* dcmd = commandCast<const DrawIndirectCountCmd*>(command_.back()); dcmd) {
			auto cmdSize = dcmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto count = hookState->indirectCommandCount;
			span = span.subspan(4, count * cmdSize); // skip the u32 count
			displayMultidraw(count, dcmd->indexed, span);
		}
	}

	// TODO: WIP
	if(command_.back()->category() == CommandCategory::dispatch) {
		auto* dcmd = deriveCast<const DispatchCmdBase*>(command_.back());
		if(ImGui::Button("Debug shader")) {
			if(dcmd->state->pipe) {
				auto mod = copySpecializeSpirv(dcmd->state->pipe->stage);
				shaderDebugger_.select(std::move(mod));
				view_ = IOView::shader;
				doUpdateHook_ = true;
				return;
			}
		}
	}

	command_.back()->displayInspector(*gui_);

	// TODO: ugly here, might happen when displayInspector changes selction...
	// not sure how to handle this better
	if(command_.empty()) {
		return;
	}

#ifdef VIL_COMMAND_CALLSTACKS
	auto flags = ImGuiTreeNodeFlags_FramePadding;
	if(!command_.back()->stacktrace.empty() && ImGui::TreeNodeEx("StackTrace", flags)) {
		ImGui::PushFont(gui_->monoFont);
		display(command_.back()->stacktrace);
		ImGui::PopFont();
		ImGui::TreePop();
	}
#endif // VIL_COMMAND_CALLSTACKS
}

void CommandViewer::draw(Draw& draw, bool skipList) {
	ZoneScoped;

	showBeforeCheckbox_ = false;

	if(command_.empty()) {
		imGuiText("No command selected");
		return;
	}

	auto& bcmd = *command_.back();
	auto actionCmd = bcmd.category() == CommandCategory::dispatch ||
		bcmd.category() == CommandCategory::draw ||
		bcmd.category() == CommandCategory::traceRays ||
		bcmd.category() == CommandCategory::transfer;

	dlg_assert(actionCmd || view_ == IOView::command);
	if(!actionCmd) {
		displayCommand();
		return;
	}

	displayActionInspector(draw, skipList);
}

void CommandViewer::updateHook() {
	auto& sel = selection();

	// we don't update any hook ops when viewing a local capture
	if(sel.updateMode() == CommandSelection::UpdateMode::localCapture) {
		return;
	}

	auto& hook = *gui_->dev().commandHook;
	auto* cmd = command_.empty() ? nullptr : command_.back();
	auto stateCmd = dynamic_cast<const StateCmdBase*>(cmd);
	auto drawIndexedCmd = commandCast<const DrawIndexedCmd*>(cmd);
	auto drawIndirectCmd = commandCast<const DrawIndirectCmd*>(cmd);
	auto drawIndirectCountCmd = commandCast<const DrawIndirectCountCmd*>(cmd);

	auto indirectCmd = cmd && isIndirect(*cmd);
	auto indexedCmd = drawIndexedCmd ||
		(drawIndirectCmd && drawIndirectCmd->indexed) ||
		(drawIndirectCountCmd && drawIndirectCountCmd->indexed);

	CommandHook::Ops ops {};
	bool setOps = true;

	switch(view_) {
		case IOView::command:
			ops.queryTime = true;
			ops.copyIndirectCmd = indirectCmd;
			break;
		case IOView::attachment:
			ops.attachmentCopies = {{
				viewData_.attachment.id,
				viewData_.attachment.type,
				beforeCommand_
			}};
			break;
		case IOView::transferSrc:
			if(commandCast<const UpdateBufferCmd*>(cmd)) {
				// nothing to do in that case, we know the data statically
				break;
			}

			ops.transferIdx = viewData_.transfer.index;
			if(beforeCommand_) {
				ops.copyTransferSrcBefore = true;
			} else {
				ops.copyTransferSrcAfter = true;
			}
			break;
		case IOView::transferDst:
			ops.transferIdx = viewData_.transfer.index;
			if(beforeCommand_) {
				ops.copyTransferDstBefore = true;
			} else {
				ops.copyTransferDstAfter = true;
			}
			break;
		case IOView::ds: {
			dlg_assert_or(stateCmd, break);
			auto pl = stateCmd->boundPipe()->layout;
			dlg_assert_or(pl, break);
			dlg_assert_or(viewData_.ds.set < pl->descriptors.size(), break);
			auto dsl = pl->descriptors[viewData_.ds.set];
			dlg_assert_or(dsl, break);
			dlg_assert_or(viewData_.ds.binding < dsl->bindings.size(), break);
			auto& bindingLayout = dsl->bindings[viewData_.ds.binding];

			// For some descriptor types, we don't need a hook
			if(bindingLayout.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
					bindingLayout.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
				break;
			}

			DescriptorCopyOp dsCopy = {
				viewData_.ds.set, viewData_.ds.binding, viewData_.ds.elem, beforeCommand_
			};
			ops.descriptorCopies = {dsCopy};
			break;
		} case IOView::mesh:
			if(viewData_.mesh.output) {
				ops.copyXfb = true;
				ops.copyIndirectCmd = indirectCmd;
			} else {
				ops.copyVertexBuffers = true;
				ops.copyIndexBuffers = indexedCmd;
				ops.copyIndirectCmd = indirectCmd;
			}
			break;
		case IOView::pushConstants:
			// nothing to copy here, we know them staically
			break;
		case IOView::shader:
			shaderDebugger_.updateHooks(hook);
			setOps = false;
			break;
	}

	if(setOps) {
		hook.freeze.store(false);

		CommandHook::Update update;
		update.invalidate = true;
		update.newOps = std::move(ops);

		hook.updateHook(std::move(update));
		selection().clearState();
	}
}

void CommandViewer::displayImage(Draw& draw, const CopiedImage& img) {
	ImGui::Separator();

	dlg_assert(img.aspectMask);
	dlg_assert(img.image);

	auto hookState = selection().completedHookState();
	dlg_assert(hookState);
	draw.usedHookState = hookState;

	// TODO: when a new CopiedImage is displayed we could reset the
	//   color mask flags. In some cases this is desired but probably
	//   not in all.
	auto flags = ImageViewer::preserveSelection |
		ImageViewer::preserveZoomPan |
		ImageViewer::preserveReadbacks |
		ImageViewer::supportsTransferSrc;
	auto imgLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	// TODO: we should probably use information about the original
	//   range (e.g. from VkImageView if there is one; or from the
	//   copy op). But not 100% what's better for gui, a level/layer
	//   slider beginning at a number that isn't 0 might be confusing.
	auto range = img.subresRange();
	imageViewer_.select(img.image, img.extent, minImageType(img.extent),
		img.format, range, imgLayout, imgLayout, flags);
	imageViewer_.display(draw);
}

const PipelineShaderStage* CommandViewer::displayDescriptorStageSelector(
		const Pipeline& pipe, unsigned setID, unsigned bindingID,
		VkDescriptorType dsType) {

	auto sstages = stages(pipe);

	// query the stages referencing this resource
	ThreadMemScope memScope;
	auto refStages = memScope.alloc<u32>(sstages.size());
	auto stageCount = 0u;
	bool selectedValid = false;

	for(auto i = 0u; i < sstages.size(); ++i) {
		auto& stage = sstages[i];
		// Don't need to respect specialization constants here
		auto res = resource(*stage.spirv->compiled, setID, bindingID, dsType);
		if(res) {
			refStages[stageCount] = i;
			++stageCount;

			if(viewData_.ds.stage == stage.stage) {
				selectedValid = true;
			}
		}
	}

	refStages = refStages.subspan(0, stageCount);
	if(stageCount == 0u) {
		ImGui::Text("Binding not used in pipeline");
		return nullptr;
	}

	if(!selectedValid) {
		viewData_.ds.stage = sstages[refStages[0]].stage;
	}

	if(stageCount == 1u) {
		return &sstages[refStages[0]];
	}

	if(ImGui::BeginCombo("Stage", vk::name(viewData_.ds.stage))) {
		for(auto id : refStages) {
			auto name = vk::name(sstages[id].stage);
			if(ImGui::Selectable(name)) {
				viewData_.ds.stage = sstages[id].stage;
			}
		}

		ImGui::EndCombo();
	}

	for(auto& stage : sstages) {
		if(stage.stage == viewData_.ds.stage) {
			return &stage;
		}
	}

	dlg_error("unreachable");
	return nullptr;
}

CommandSelection& CommandViewer::selection() const {
	return gui_->cbGui().selector();
}

} // namespace vil
