#include <gui/command.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <util/profiling.hpp>
#include <util/buffmt.hpp>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <device.hpp>
#include <threadContext.hpp>
#include <pipe.hpp>
#include <accelStruct.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <ds.hpp>
#include <shader.hpp>
#include <rp.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <bitset>

#ifdef VIL_ENABLE_COMMAND_CALLSTACKS
	#include <util/callstack.hpp>
#endif // VIL_ENABLE_COMMAND_CALLSTACKS

// NOTE: since we might view invalidated command records, we can't assume
//   any handles in Command objects to be not null (they are unset to null
//   when the cooresponding handle is destroyed/invalidated).

namespace vil {
namespace {

#ifdef VIL_ENABLE_COMMAND_CALLSTACKS
void display(const backward::StackTrace& st, unsigned offset = 3u) {
	// TODO the static maps here are terrible
	static backward::TraceResolver resolver;
	static std::unordered_map<void*, backward::ResolvedTrace::SourceLoc> locs;

	resolver.load_stacktrace(st);

	for(auto i = offset; i < st.size(); ++i) {
		auto it = locs.find(st[i - 1].addr);
		if(it == locs.end()) {
			auto res = resolver.resolve(st[i - 1]);
			it = locs.emplace(st[i - 1].addr, res.source).first;
		}

		auto& loc = it->second;
		imGuiText("#{}: {}:{}:{}: {} [{}]", i, loc.filename, loc.line,
			loc.col, loc.function, st[i - 1].addr);
		if(ImGui::IsItemClicked()) {
			// TODO
			auto base = std::filesystem::current_path();
			auto cmd = dlg::format("nvr -c \"e +{} {}/{}\"", loc.line,
				base.string(), loc.filename);
			(void) std::system(cmd.c_str());
		}
	}
}
#endif // VIL_ENABLE_COMMAND_CALLSTACKS

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
	record_ = {};
	state_ = {};
	command_ = nullptr;

	view_ = IOView::command;
	viewData_.command.selected = 0;
}

void CommandViewer::select(IntrusivePtr<CommandRecord> rec, const Command& cmd,
		CommandDescriptorSnapshot dsState, bool resetState,
		IntrusivePtr<CommandHookState> newState) {

	const DrawCmdBase* drawCmd {};
	const StateCmdBase* stateCmd {};

	switch(cmd.type()) {
		case CommandType::draw:
			drawCmd = deriveCast<const DrawCmdBase*>(&cmd);
			stateCmd = drawCmd;
			break;
		case CommandType::dispatch:
			stateCmd = deriveCast<const DispatchCmdBase*>(&cmd);
			break;
		case CommandType::traceRays:
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
			if(!command_) {
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

			auto* lastDrawCmd = dynamic_cast<const DrawCmdBase*>(command_);
			dlg_assert(lastDrawCmd && lastDrawCmd->state.rpi);
			dlg_assert(drawCmd->state.rpi);
			auto& rpiNew = *drawCmd->state.rpi;
			auto& rpiOld = *lastDrawCmd->state.rpi;

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

			auto* lastStateCmd = deriveCast<const StateCmdBase*>(command_);
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
		case IOView::transferDst:
			// Only keep transferSrc/Dst selection when the command type
			// is exactly the same
			if(!command_ || typeid(*command_) != typeid(cmd)) {
				selectCommandView = true;
			}

			// always reset this
			viewData_.transfer.index = 0u;
			break;
		case IOView::pushConstants: {
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

				auto refl = accessReflection(stage);
				if(!refl.get().get_shader_resources().push_constant_buffers.empty()) {
					selectCommandView = false;
				}

				break;
			}

			break;
		} case IOView::shader: {
			auto* lastStateCmd = dynamic_cast<const StateCmdBase*>(command_);
			dlg_assert(lastStateCmd);

			if(!stateCmd || !stateCmd->boundPipe()
					|| stateCmd->boundPipe() != lastStateCmd->boundPipe()) {
				selectCommandView = true;
				shaderDebugger_.unselect();
			}

			break;
		}
	}

	record_ = rec;
	command_ = &cmd;
	dsState_ = std::move(dsState);

	if(selectCommandView) {
		view_ = IOView::command;
		viewData_.command = {};
		resetState = true;
	}

	if(resetState) {
		state_ = {};
		imageViewer_.reset();

		// Even when we could keep our selection, when resetState is true
		// the command might have potentially changed (e.g. from a Draw
		// command to a DrawIndirect command), requiring us to update
		// the hook ops.
		updateHook();
	} else if(newState) {
		state_ = newState;
	}
}

void CommandViewer::displayTransferIOList() {
	auto& cmd = nonNull(command_);
	dlg_assert(cmd.type() == CommandType::transfer);

	// TODO: add support for viewing buffers here.
	// Hard to do in a meaningful way though.
	auto found = false;
	auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf |
		ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;

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
		if(ImGui::IsItemClicked()) {
			view_ = IOView::transferSrc;
			viewData_.transfer.index = 0u; // always reset
			imageViewer_.reset();
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
		if(ImGui::IsItemClicked()) {
			view_ = IOView::transferDst;
			viewData_.transfer.index = 0u; // always reset
			imageViewer_.reset();
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

	addSrcDst(dynamic_cast<const CopyImageCmd*>(&cmd));
	addSrcDst(dynamic_cast<const CopyBufferToImageCmd*>(&cmd));
	addSrcDst(dynamic_cast<const CopyImageToBufferCmd*>(&cmd));
	addSrcDst(dynamic_cast<const BlitImageCmd*>(&cmd));
	addSrcDst(dynamic_cast<const ResolveImageCmd*>(&cmd));
	addSrcDst(dynamic_cast<const CopyBufferCmd*>(&cmd));
	addSrcDst(dynamic_cast<const UpdateBufferCmd*>(&cmd));
	addDst(dynamic_cast<const ClearColorImageCmd*>(&cmd));
	addDst(dynamic_cast<const ClearDepthStencilImageCmd*>(&cmd));
	addDst(dynamic_cast<const FillBufferCmd*>(&cmd));

	if(auto* ca = dynamic_cast<const ClearAttachmentCmd*>(&cmd)) {
		found = true;

		for(auto [i, att] : enumerate(ca->attachments)) {
			auto lflags = flags;
			if(view_ == IOView::transferDst && viewData_.transfer.index == i) {
				lflags |= ImGuiTreeNodeFlags_Selected;
			}

			auto label = dlg::format("Attachment {}", att.colorAttachment);
			ImGui::TreeNodeEx(label.c_str(), lflags);
			if(ImGui::IsItemClicked()) {
				view_ = IOView::transferDst;
				viewData_.transfer.index = i;
				imageViewer_.reset();
				updateHook();
			}
		}
	}

	dlg_assertlm(dlg_level_warn, found, "IO inspector unimplemented for command");
}

void CommandViewer::displayDsList() {
	auto& baseCmd = nonNull(command_);

	const StateCmdBase* cmd {};
	const DrawCmdBase* drawCmd {};
	const DispatchCmdBase* dispatchCmd {};
	const TraceRaysCmdBase* traceCmd {};

	switch(baseCmd.type()) {
		case CommandType::draw:
			drawCmd = deriveCast<const DrawCmdBase*>(&baseCmd);
			cmd = drawCmd;
			break;
		case CommandType::dispatch:
			dispatchCmd = deriveCast<const DispatchCmdBase*>(&baseCmd);
			cmd = dispatchCmd;
			break;
		case CommandType::traceRays:
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
	if(ImGui::TreeNodeEx("Descriptors", ImGuiTreeNodeFlags_FramePadding)) {
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
			auto stateIt = dsState_.states.find(ds.dsEntry);
			dlg_assert_or(stateIt != dsState_.states.end(), continue);

			auto& dsCow = *stateIt->second;

			auto label = dlg::format("Descriptor Set {}", setID);
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			if(ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_FramePadding)) {
				auto [state, lock] = access(dsCow);
				for(auto bID = 0u; bID < state.layout->bindings.size(); ++bID) {
					auto sstages = stages(pipe);
					dlg_assert(!sstages.empty());

					ThreadMemScope memScope;
					auto stageNames = memScope.alloc<std::string>(sstages.size());

					std::optional<u32> firstValid;
					for(auto i = 0u; i < sstages.size(); ++i) {
						auto& stage = sstages[i];
						auto refl = accessReflection(stage);
						auto name = bindingName(refl.get(), setID, bID);
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

					auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf |
						ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;
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
					if(ImGui::IsItemClicked()) {
						view_ = IOView::ds;
						viewData_.ds = {setID, bID, 0, VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM};
						imageViewer_.reset();
						updateHook();
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
	auto& cmd = nonNull(command_);

	const StateCmdBase* stateCmd {};
	const DrawCmdBase* drawCmd {};
	const DispatchCmdBase* dispatchCmd {};
	const TraceRaysCmdBase* traceCmd {};

	switch(cmd.type()) {
		case CommandType::draw:
			drawCmd = deriveCast<const DrawCmdBase*>(&cmd);
			stateCmd = drawCmd;
			break;
		case CommandType::dispatch:
			dispatchCmd = deriveCast<const DispatchCmdBase*>(&cmd);
			stateCmd = dispatchCmd;
			break;
		case CommandType::traceRays:
			traceCmd = deriveCast<const TraceRaysCmdBase*>(&cmd);
			stateCmd = traceCmd;
			break;
		default:
			break;
	}

	(void) traceCmd;

	auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
		ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if(view_ == IOView::command) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::TreeNodeEx("Command", flags);
	if(ImGui::IsItemClicked()) {
		view_ = IOView::command;
		updateHook();
	}

	// Transfer IO
	if(cmd.type() == CommandType::transfer) {
		displayTransferIOList();
		return;
	}

	// Vertex IO
	if(drawCmd) {
		auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
			ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if(view_ == IOView::mesh) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		ImGui::TreeNodeEx("Vertex input", flags);
		if(ImGui::IsItemClicked()) {
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
		if(ImGui::TreeNodeEx("Attachments", ImGuiTreeNodeFlags_FramePadding)) {
			if(drawCmd->state.rpi) {
				auto& rpi = *drawCmd->state.rpi;
				auto addAttachment = [&](const std::string& label, AttachmentType type,
						unsigned id, ImageView* view) {
					if(!view) {
						return;
					}

					auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf |
						ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;
					if(view_ == IOView::attachment &&
							viewData_.attachment.type == type &&
							viewData_.attachment.id == id) {
						flags |= ImGuiTreeNodeFlags_Selected;
					}

					ImGui::TreeNodeEx(label.c_str(), flags);
					if(ImGui::IsItemClicked()) {
						view_ = IOView::attachment;
						viewData_.attachment = {type, id};
						imageViewer_.reset();
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
			auto refl = accessReflection(stage);
			auto& compiled = refl.get();
			if(compiled.get_shader_resources().push_constant_buffers.empty()) {
				continue;
			}

			auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf |
				ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;
			if(viewPCRStage == stage.stage) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			auto stageName = vk::name(stage.stage);
			auto label = sstages.size() == 1u ?
				"Push Constants" : dlg::format("Push Constants {}", stageName);
			ImGui::TreeNodeEx(label.c_str(), flags);
			if(ImGui::IsItemClicked()) {
				view_ = IOView::pushConstants;
				viewData_.pushConstants = {stage.stage};
				updateHook();
			}
		}
	}
}

bool CommandViewer::displayBeforeCheckbox() {
	if(ImGui::Checkbox("Before Command", &beforeCommand_)) {
		updateHook();
		return true;
	}

	return false;
}

void CommandViewer::displayDs(Draw& draw) {
	auto& gui = *this->gui_;

	auto* cmd = deriveCast<const StateCmdBase*>(command_);
	dlg_assert_or(cmd, return);

	if(!cmd->boundPipe()) {
		ImGui::Text("Pipeline was destroyed, can't interpret content");
		return;
	}

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

	auto stateIt = dsState_.states.find(setEntry);
	dlg_assert_or(stateIt != dsState_.states.end(), return);

	auto& dsCow = *stateIt->second;
	auto [state, lock] = access(dsCow);

	if(bindingID >= state.layout->bindings.size()) {
		ImGui::Text("Binding not bound");
		dlg_warn("Binding not bound? Shouldn't happen");
		return;
	}

	auto& bindingLayout = state.layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	auto dsCat = category(dsType);

	// NOTE: the "Before Command" checkbox might not make sense for other types.
	// For uniform buffers etc. the contents *should* stay the same but it
	// might still be nice to select when they are captured, for some
	// debugging corner cases. For sampler, otoh, we just don't capture
	// anything anyways.
	if(dsType != VK_DESCRIPTOR_TYPE_SAMPLER) {
		if(displayBeforeCheckbox()) {
			ImGui::Text("Updating...");
			return;
		}
	}

	auto& elemID = viewData_.ds.elem;
	if(dsCat != DescriptorCategory::inlineUniformBlock) {
		auto bindingCount = descriptorCount(state, bindingID);
		if(optSliderRange("Element", elemID, bindingCount)) {
			updateHook();
			state_ = {};
		}

		if(elemID >= bindingCount) {
			ImGui::Text("Element not bound");
			dlg_warn("Element not bound? Shouldn't happen");
			return;
		}
	}

	// TODO: we expect descriptors to be valid here. Needs rework
	// for descriptor indexing

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
		auto& elem = buffers(state, bindingID)[elemID];
		auto& srcBuf = nonNull(elem.buffer);
		refButton(gui, srcBuf);
		ImGui::SameLine();
		drawOffsetSize(elem, dynOffset);

		// interpret content
		if(!state_) {
			ImGui::Text("Waiting for a submission...");
			return;
		}

		if(state_->copiedDescriptors.empty()) {
			dlg_error("copiedDescriptors shouldn't be empty");
			ImGui::Text("Error copying descriptor. See log output");
			return;
		}

		dlg_assert(state_->copiedDescriptors.size() == 1u);
		auto* buf = std::get_if<OwnBuffer>(&state_->copiedDescriptors[0].data);
		if(!buf) {
			dlg_assert(state_->copiedDescriptors[0].data.index() == 0);
			imGuiText("Error copying descriptor buffer. See log output");
			return;
		}

		auto* stage = displayDescriptorStageSelector(pipe, setID, bindingID, dsType);
		if(stage) {
			auto refl = accessReflection(*stage);
			auto& compiled = refl.get();
			// TODO: reuse found resource from displayDescriptorStageSelector
			auto res = nonNull(resource(compiled, setID, bindingID, dsType));
			auto name = compiled.get_name(res.id);
			if(name.empty()) {
				name = compiled.get_name(res.base_type_id);
				if(name.empty()) {
					name = "?";
				}
			}

			ThreadMemScope memScope;
			auto* type = buildType(compiled, res.type_id, memScope.customUse());
			displayTable(name.c_str(), *type, buf->data());
		} else {
			ImGui::Text("Binding not used in pipeline");
		}
	} else if(dsCat == DescriptorCategory::image) {
		imGuiText("{}", vk::name(dsType));

		// == Sampler ==
		if(needsSampler(dsType)) {
			if(bindingLayout.immutableSamplers) {
				refButtonExpect(gui, bindingLayout.immutableSamplers[elemID].get());
			} else {
				auto& elem = images(state, bindingID)[elemID];
				refButtonExpect(gui, elem.sampler);
			}
		}

		// == Image ==
		if(needsImageView(dsType)) {
			// general info
			auto& elem = images(state, bindingID)[elemID];
			auto& imgView = nonNull(elem.imageView);
			refButton(gui, imgView);

			ImGui::SameLine();
			refButtonD(gui, imgView.img);

			// imgView.img can be null if the image was destroyed. This isn't
			// too unlikely since we might have kept the imgView alive since it's
			// still referenced in our DescriptorState
			if(imgView.img) {
				imGuiText("Format: {}", vk::name(imgView.ci.format));
				auto& extent = imgView.img->ci.extent;
				imGuiText("Extent: {}x{}x{}", extent.width, extent.height, extent.depth);

				if(needsImageLayout(dsType)) {
					imGuiText("Layout: {}", vk::name(elem.layout));
				}

				// content
				if(!state_) {
					ImGui::Text("Waiting for a submission...");
					return;
				}

				if(state_->copiedDescriptors.empty()) {
					dlg_error("copiedDescriptors shouldn't be empty");
					ImGui::Text("Error copying descriptor. See log output");
					return;
				}

				dlg_assert(state_->copiedDescriptors.size() == 1u);
				auto* img = std::get_if<CopiedImage>(&state_->copiedDescriptors[0].data);
				if(!img) {
					dlg_assert(state_->copiedDescriptors[0].data.index() == 0);
					imGuiText("Error copying descriptor image. See log output");
					return;
				}

				displayImage(draw, *img);
			}
		}
	} else if(dsCat == DescriptorCategory::bufferView) {
		imGuiText("TODO: bufferView viewer not implemented yet");
	} else if(dsCat == DescriptorCategory::accelStruct) {
		auto& elem = accelStructs(state, bindingID)[elemID];
		refButtonExpect(gui, elem.accelStruct);
		// TODO: show data of acceleration structure?
	} else if(dsCat == DescriptorCategory::inlineUniformBlock) {
		auto blockData = inlineUniformBlock(state, bindingID);
		imGuiText("Inline Uniform Block, Size {}", blockData.size());

		auto* stage = displayDescriptorStageSelector(pipe, setID, bindingID, dsType);
		if(stage) {
			auto refl = accessReflection(*stage);
			auto& compiled = refl.get();
			// TODO: reuse found resource from displayDescriptorStageSelector
			auto res = nonNull(resource(compiled, setID, bindingID, dsType));
			auto name = compiled.get_name(res.id);
			if(name.empty()) {
				name = compiled.get_name(res.base_type_id);
				if(name.empty()) {
					name = "?";
				}
			}

			ThreadMemScope memScope;
			auto* type = buildType(compiled, res.type_id, memScope.customUse());
			displayTable(name.c_str(), *type, blockData);
		} else {
			ImGui::Text("Binding not used in pipeline");
		}
	} else {
		imGuiText("Unsupported descriptoer type {}", vk::name(dsType));
	}
}

void CommandViewer::displayAttachment(Draw& draw) {
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(command_);
	dlg_assert_or(drawCmd, return);

	// NOTE: maybe only show this button for output attachments (color, depthStencil)?
	// Does not make sense otherwise as it stays the same i guess.
	// But could be useful for debugging nonetheless
	if(displayBeforeCheckbox()) {
		ImGui::Text("Updating...");
		return;
	}

	dlg_assert(drawCmd->state.rpi);
	auto& rpi = *drawCmd->state.rpi;

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

	refButtonD(*gui_, attachments[aid]);
	if(attachments[aid]) {
		refButtonD(*gui_, attachments[aid]->img);
	}

	if(state_) {
		if(state_->copiedAttachments.empty()) {
			dlg_error("copiedAttachments should not be empty");
			ImGui::Text("No attachment copy found. See log output");
			return;
		}

		dlg_assert(state_->copiedAttachments.size() == 1u);
		if(state_->copiedAttachments[0].data.image) {
			displayImage(draw, state_->copiedAttachments[0].data);
		} else {
			imGuiText("Error copying attachment. See log output");
		}
	} else {
		ImGui::Text("Waiting for a submission...");
	}
}

void CommandViewer::displayPushConstants() {
	auto* cmd = deriveCast<const StateCmdBase*>(command_);
	dlg_assert_or(cmd, return);

	if(!cmd->boundPipe()) {
		ImGui::Text("Pipeline was destroyed, can't interpret push constants");
		return;
	}

	auto viewStage = viewData_.pushConstants.stage;
	auto found = false;

	for(auto& stage : stages(*cmd->boundPipe())) {
		if(stage.stage != viewStage) {
			continue;
		}

		found = true;
		auto refl = accessReflection(stage);
		auto& compiled = refl.get();
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
	auto& cmd = nonNull(command_);

	if(!state_) {
		ImGui::Text("Waiting for a submission...");
		return;
	}

	// NOTE: only show where it makes sense?
	// shouldn't be here for src resources i guess.
	// But could be useful for debugging anyways
	if(displayBeforeCheckbox()) {
		ImGui::Text("Updating...");
		return;
	}

	dlg_assert(view_ == IOView::transferSrc || view_ == IOView::transferDst);
	bool refBuffer = false;
	bool refImage = false;
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
			refBuffer = true;
		} else {
			static_assert(std::is_convertible_v<decltype(ccmd->dst), const Image*>);
			refImage = true;
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
			refBuffer = true;
		} else {
			static_assert(std::is_convertible_v<decltype(ccmd->src), const Image*>);
			refImage = true;
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

	refSrcDst(dynamic_cast<const CopyImageCmd*>(&cmd));
	refSrcDst(dynamic_cast<const CopyBufferToImageCmd*>(&cmd));
	refSrcDst(dynamic_cast<const CopyImageToBufferCmd*>(&cmd));
	refSrcDst(dynamic_cast<const BlitImageCmd*>(&cmd));
	refSrcDst(dynamic_cast<const ResolveImageCmd*>(&cmd));

	refSrcDst(dynamic_cast<const CopyBufferCmd*>(&cmd));
	refDst(dynamic_cast<const FillBufferCmd*>(&cmd));
	refDst(dynamic_cast<const ClearColorImageCmd*>(&cmd));
	refDst(dynamic_cast<const ClearDepthStencilImageCmd*>(&cmd));

	if(auto* ccmd = dynamic_cast<const UpdateBufferCmd*>(&cmd); ccmd) {
		if(view_ == IOView::transferSrc) {
			ImGui::Separator();
			imGuiText("Static data of size {}", ccmd->data.size());
			bufferViewer_.display(state_->transferBufCopy.data());
			return;
		} else  {
			refDst(ccmd);
		}
	}

	dlg_assert(refBuffer ^ refImage);
	dlg_assert(tcount > 0u);

	if(optSliderRange("Transfer", viewData_.transfer.index, tcount)) {
		updateHook();
		state_ = {};
		imGuiText("Updating...");
		return;
	}

	dlg_assert(refBuffer == !!state_->transferBufCopy.buf);
	dlg_assert(refImage == !!state_->transferImgCopy.image);

	if(refBuffer && state_->transferBufCopy.buf) {
		bufferViewer_.display(state_->transferBufCopy.data());
	} else if(refImage && state_->transferImgCopy.image) {
		displayImage(draw, state_->transferImgCopy);
	} else {
		imGuiText("Error copying data. See log output");
	}
}

void CommandViewer::displayVertexViewer(Draw& draw) {
	auto& cmd = nonNull(command_);
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	dlg_assert_or(drawCmd, return);

	if(!drawCmd->state.pipe) {
		ImGui::Text("Pipeline was destroyed, can't interpret state");
		return;
	} else if(!state_) {
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
				vertexViewer_.displayInput(draw, *drawCmd, *state_, gui_->dt());
			}

			ImGui::EndTabItem();
		}

		if(ImGui::BeginTabItem("Vertex Output")) {
			if(!viewData_.mesh.output) {
				viewData_.mesh.output = true;
				updateHook();
			} else {
				vertexViewer_.updateInput(gui_->dt());
				vertexViewer_.displayOutput(draw, *drawCmd, *state_, gui_->dt());
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

void CommandViewer::displayActionInspector(Draw& draw) {
	// TODO: don't even display the inspector when we are viewing a static
	// record and that record is invalidated.
	// (or other cases where we know it will never be submitted again)
	// auto& hook = *gui.dev().commandHook;
	// if(hook ... invalid?)
	// 	return true;
	// }

	ZoneScoped;

	auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
	if(!ImGui::BeginTable("IO inspector", 2, flags, ImGui::GetContentRegionAvail())) {
		return;
	}

	ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, 200.f);
	ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	ImGui::BeginChild("Command IO list");

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 4.f));

	displayIOList();

	ImGui::PopStyleVar(2);

	ImGui::EndChild();
	ImGui::TableNextColumn();
	ImGui::BeginChild("Command IO Inspector");

	displaySelectedIO(draw);

	ImGui::EndChild();
	ImGui::EndTable();
}

void CommandViewer::displayCommand() {
	dlg_assert(view_ == IOView::command);

	if(state_) {
		dlg_assert(record_);

		auto lastTime = state_->neededTime;
		auto validBits = gui_->dev().queueFamilies[record_->queueFamily].props.timestampValidBits;
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

	if(state_ && state_->indirectCopy.size) {
		auto& ic = state_->indirectCopy;
		auto span = ic.data();
		if(auto* dcmd = dynamic_cast<const DrawIndirectCmd*>(command_); dcmd) {
			displayMultidraw(dcmd->drawCount, dcmd->indexed, span);
		} else if(dynamic_cast<const DispatchIndirectCmd*>(command_)) {
			auto ecmd = read<VkDispatchIndirectCommand>(span);
			imGuiText("groups X: {}", ecmd.x);
			imGuiText("groups Y: {}", ecmd.y);
			imGuiText("groups Z: {}", ecmd.z);
		} else if(dynamic_cast<const TraceRaysIndirectCmd*>(command_)) {
			auto ecmd = read<VkTraceRaysIndirectCommandKHR>(span);
			imGuiText("width: {}", ecmd.width);
			imGuiText("height: {}", ecmd.height);
			imGuiText("depth: {}", ecmd.depth);
		} else if(auto* dcmd = dynamic_cast<const DrawIndirectCountCmd*>(command_); dcmd) {
			auto cmdSize = dcmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto count = state_->indirectCommandCount;
			span = span.subspan(4, count * cmdSize); // skip the u32 count
			displayMultidraw(count, dcmd->indexed, span);
		}
	}

	// TODO: WIP
	if(auto* dcmd = dynamic_cast<const DispatchCmdBase*>(command_); dcmd) {
		if(ImGui::Button("Debug shader")) {
			if(dcmd->state.pipe) {
				// TODO: set specialization constants
				auto& spirv = nonNull(dcmd->state.pipe->stage.spirv);
				shaderDebugger_.select(nonNull(spirv.compiled));
				view_ = IOView::shader;
				updateHook();
				return;
			}
		}
	}

	command_->displayInspector(*gui_);

#ifdef VIL_ENABLE_COMMAND_CALLSTACKS
	auto flags = ImGuiTreeNodeFlags_FramePadding;
	if(command_->stackTrace && ImGui::TreeNodeEx("StackTrace", flags)) {
		vil::display(*command_->stackTrace);
		ImGui::TreePop();
	}
#endif // VIL_ENABLE_COMMAND_CALLSTACKS
}

void CommandViewer::draw(Draw& draw) {
	ZoneScoped;

	if(!command_) {
		imGuiText("No command selected");
		return;
	}

	dlg_assert(record_);

	// we read a lot of data from commands in the given record, so make
	// sure there aren't any invalid handles in there.
	if(!record_->cb) {
		replaceInvalidatedLocked(*record_);
	}

	dlg_assert(record_->invalidated.empty());

	auto& bcmd = nonNull(command_);
	auto actionCmd = bcmd.type() == CommandType::dispatch ||
		bcmd.type() == CommandType::draw ||
		bcmd.type() == CommandType::traceRays ||
		bcmd.type() == CommandType::transfer;

	dlg_assert(actionCmd || view_ == IOView::command);
	if(!actionCmd) {
		displayCommand();
		return;
	}

	displayActionInspector(draw);
}

void CommandViewer::updateHook() {
	auto& hook = *gui_->dev().commandHook;
	hook.unsetHookOps();
	state_ = {};

	auto stateCmd = dynamic_cast<const StateCmdBase*>(command_);
	auto drawIndexedCmd = dynamic_cast<const DrawIndexedCmd*>(command_);
	auto drawIndirectCmd = dynamic_cast<const DrawIndirectCmd*>(command_);
	auto dispatchIndirectCmd = dynamic_cast<const DispatchIndirectCmd*>(command_);
	auto drawIndirectCountCmd = dynamic_cast<const DrawIndirectCountCmd*>(command_);
	auto traceRaysIndirectCmd = dynamic_cast<const TraceRaysIndirectCmd*>(command_);

	auto indirectCmd = dispatchIndirectCmd ||
		drawIndirectCmd ||
		drawIndirectCountCmd ||
		traceRaysIndirectCmd;
	auto indexedCmd = drawIndexedCmd ||
		(drawIndirectCmd && drawIndirectCmd->indexed) ||
		(drawIndirectCountCmd && drawIndirectCountCmd->indexed);

	hook.freeze = false;

	switch(view_) {
		case IOView::command:
			hook.queryTime = true;
			hook.copyIndirectCmd = indirectCmd;
			break;
		case IOView::attachment:
			hook.attachmentCopies = {{
				viewData_.attachment.type,
				viewData_.attachment.id,
				beforeCommand_
			}};
			break;
		case IOView::transferSrc:
			if(dynamic_cast<const UpdateBufferCmd*>(command_)) {
				// nothing to do in that case, we know the data statically
				break;
			}

			hook.transferIdx = viewData_.transfer.index;
			hook.copyTransferSrc = true;
			hook.copyTransferBefore = beforeCommand_;
			break;
		case IOView::transferDst:
			hook.transferIdx = viewData_.transfer.index;
			hook.copyTransferDst = true;
			hook.copyTransferBefore = beforeCommand_;
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

			CommandHook::DescriptorCopy dsCopy = {
				viewData_.ds.set, viewData_.ds.binding, viewData_.ds.elem, beforeCommand_
			};
			hook.descriptorCopies = {dsCopy};
			break;
		} case IOView::mesh:
			if(viewData_.mesh.output) {
				hook.copyXfb = true;
				hook.copyIndirectCmd = indirectCmd;
			} else {
				hook.copyVertexBuffers = true;
				hook.copyIndexBuffers = indexedCmd;
				hook.copyIndirectCmd = indirectCmd;
			}
			break;
		case IOView::pushConstants:
			// nothing to copy here, we know them staically
			break;
		case IOView::shader:
			shaderDebugger_.updateHooks(hook);
			break;
	}
}

void CommandViewer::displayImage(Draw& draw, const CopiedImage& img) {
	ImGui::Separator();

	dlg_assert(img.aspectMask);
	dlg_assert(img.image);
	dlg_assert(state_);

	draw.usedHookState = state_;

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
		auto refl = accessReflection(stage);
		auto res = resource(refl.get(), setID, bindingID, dsType);
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

} // namespace vil
