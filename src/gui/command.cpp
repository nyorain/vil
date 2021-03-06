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

// NOTE: since we might view invalidated command records, we can't assume
//   any handles in Command objects to be not null (they are unset to null
//   when the cooresponding handle is destroyed/invalidated).

namespace vil {

// CommandViewer
CommandViewer::CommandViewer() = default;
CommandViewer::~CommandViewer() = default;

Device& CommandViewer::dev() const {
	return gui_->dev();
}

void CommandViewer::init(Gui& gui) {
	gui_ = &gui;
	vertexViewer_.init(gui.dev(), gui.rp());
}

void CommandViewer::unselect() {
	record_ = {};
	state_ = {};
	command_ = nullptr;

	view_ = IOView::command;
	viewData_.command.selected = 0;
}

void CommandViewer::select(IntrusivePtr<CommandRecord> rec, const Command& cmd,
		CommandDescriptorSnapshot dsState, bool resetState) {

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

			auto id = viewData_.attachment.id;
			auto* lastDrawCmd = dynamic_cast<const DrawCmdBase*>(command_);
			dlg_assert(lastDrawCmd && lastDrawCmd->state.rpi.rp);

			// when the newly select command uses a different renderpass,
			// we reset the selection, otherwise we keep it.
			// NOTE: would probably be better to compare for render pass
			// compatibility instead of identity equality?
			if(!lastDrawCmd->state.rpi.rp || !drawCmd->state.rpi.rp ||
					lastDrawCmd->state.rpi.rp != drawCmd->state.rpi.rp) {
				selectCommandView = true;
			} else {
				dlg_assert(id < drawCmd->state.rpi.rp->desc.attachments.size());
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
			break;
		case IOView::pushConstants:
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
		ioImage_ = {};

		// Even when we could keep our selection, when resetState is true
		// the command might have potentially changed (e.g. from a Draw
		// command to a DrawIndirect command), requiring us to update
		// the hook ops.
		updateHook();
	}
}

void CommandViewer::state(IntrusivePtr<CommandHookState> state) {
	state_ = state;
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
			ioImage_ = {};
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
			ioImage_ = {};
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
	addDst(dynamic_cast<const CopyBufferToImageCmd*>(&cmd));
	addSrc(dynamic_cast<const CopyImageToBufferCmd*>(&cmd));
	addSrcDst(dynamic_cast<const BlitImageCmd*>(&cmd));
	addSrcDst(dynamic_cast<const ResolveImageCmd*>(&cmd));
	// addSrcDst(dynamic_cast<const CopyBufferCmd*>(&cmd));
	// addDst(dynamic_cast<const UpdateBufferCmd*>(&cmd));
	// addDst(dynamic_cast<const FillBufferCmd*>(&cmd));
	addDst(dynamic_cast<const ClearColorImageCmd*>(&cmd));
	addDst(dynamic_cast<const ClearDepthStencilImageCmd*>(&cmd));

	// TODO: add support for selecting one of the multiple
	//   cleared attachments. We need special treatment here anyways,
	//   need to set copyAttachment in updateHook for this.
	// addDst(dynamic_cast<const ClearAttachmentCmd*>(&cmd));

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
		for(auto setID = 0u; setID < dss.size(); ++setID) {
			auto& ds = dss[setID];

			// No descriptor set bound
			if(!ds.ds) {
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

			auto stateIt = dsState_.states.find(ds.ds);
			dlg_assert_or(stateIt != dsState_.states.end(), continue);

			auto& state = *stateIt->second;
			dlg_assert(state.layout);

			auto label = dlg::format("Descriptor Set {}", setID);
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			if(ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_FramePadding)) {
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
							if(!firstValid) {
								firstValid = i;
							}
						} else if(name.type == BindingNameRes::Type::unnamed) {
							stageNames[i] = "<unnamed>";
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
						ioImage_ = {};
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
			if(drawCmd->state.rpi.rp) {
				auto& desc = drawCmd->state.rpi.rp->desc;
				auto subpassID = drawCmd->state.rpi.subpass;
				dlg_assert(subpassID < desc.subpasses.size());

				auto& subpass = desc.subpasses[subpassID];

				auto addAttachment = [&](auto label, auto id) {
					if(id == VK_ATTACHMENT_UNUSED) {
						return;
					}

					auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf |
						ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;
					if(view_ == IOView::attachment && viewData_.attachment.id == id) {
						flags |= ImGuiTreeNodeFlags_Selected;
					}

					ImGui::TreeNodeEx(label.c_str(), flags);
					if(ImGui::IsItemClicked()) {
						view_ = IOView::attachment;
						viewData_.attachment = {id};
						ioImage_ = {};
						updateHook();
					}
				};

				// TODO: name them if possible. Could use names from (fragment)
				// shader. Or names of image/imageView?
				for(auto c = 0u; c < subpass.colorAttachmentCount; ++c) {
					auto label = dlg::format("Color Attachment {}", c);
					addAttachment(label, subpass.pColorAttachments[c].attachment);
				}

				for(auto i = 0u; i < subpass.inputAttachmentCount; ++i) {
					auto label = dlg::format("Input Attachment {}", i);
					addAttachment(label, subpass.pInputAttachments[i].attachment);
				}

				if(subpass.pDepthStencilAttachment) {
					auto label = dlg::format("Depth Stencil");
					addAttachment(label, subpass.pDepthStencilAttachment->attachment);
				}

				// NOTE: display preserve attachments? resolve attachments?
			} else {
				imGuiText("Could not get render pass. Was it destroyed?");
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
	if(displayBeforeCheckbox()) {
		ImGui::Text("Updating...");
		return;
	}

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
		ImGui::Text("DescriptorSet not bound");
		dlg_warn("DescriptorSet not bound? Shouldn't happen");
		return;
	}

	auto* set = dss[setID].ds;
	if(!set) {
		ImGui::Text("DescriptorSet was not bound or destroyed/invalidated");
		return;
	}

	auto stateIt = dsState_.states.find(set);
	dlg_assert_or(stateIt != dsState_.states.end(), return);

	auto& state = *stateIt->second;
	dlg_assert(state.layout);

	if(bindingID >= state.layout->bindings.size()) {
		ImGui::Text("Binding not bound");
		dlg_warn("Binding not bound? Shouldn't happen");
		return;
	}

	auto& bindingLayout = state.layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	auto dsCat = category(dsType);

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

		auto* buf = std::get_if<OwnBuffer>(&state_->dsCopy);
		if(!buf) {
			dlg_assert(state_->dsCopy.index() == 0);
			imGuiText("Error: {}", state_->errorMessage);
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

			auto flags = ImGuiTableFlags_BordersInner |
				ImGuiTableFlags_Resizable |
				ImGuiTableFlags_SizingStretchSame;
			if(ImGui::BeginTable("Values", 2u, flags)) {
				ImGui::TableSetupColumn(nullptr, 0, 0.25f);
				ImGui::TableSetupColumn(nullptr, 0, 0.75f);

				display(compiled, res.type_id, name.c_str(), buf->data());
				ImGui::EndTable();
			}
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
				refButtonExpect(gui, elem.sampler.get());
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

				auto* img = std::get_if<CopiedImage>(&state_->dsCopy);
				if(!img) {
					dlg_assert(state_->dsCopy.index() == 0);
					imGuiText("Error: {}", state_->errorMessage);
					return;
				}

				displayImage(draw, *img);
			}
		}
	} else if(dsCat == DescriptorCategory::bufferView) {
		imGuiText("TODO: bufferView viewer not implemented yet");
	} else if(dsCat == DescriptorCategory::accelStruct) {
		auto& elem = accelStructs(state, bindingID)[elemID];
		refButtonExpect(gui, elem.get());
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

			auto flags = ImGuiTableFlags_BordersInner |
				ImGuiTableFlags_Resizable |
				ImGuiTableFlags_SizingStretchSame;
			if(ImGui::BeginTable("Values", 2u, flags)) {
				ImGui::TableSetupColumn(nullptr, 0, 0.25f);
				ImGui::TableSetupColumn(nullptr, 0, 0.75f);

				display(compiled, res.type_id, name.c_str(), blockData);
				ImGui::EndTable();
			}
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

	if(drawCmd->state.rpi.attachments.empty()) {
		// attachments were destroyed, or we have a secondary cb that didn't
		// se the framebuffer in inheritInfo
		// NOTE: for secondary records (that are only nested to the main
		// record we are viewing) we could figure it out. Not sure if worth
		// it.
		imGuiText("No attachment information");
		return;
	}

	// information
	auto aid = viewData_.attachment.id;
	auto& attachments = drawCmd->state.rpi.attachments;
	dlg_assert(aid < attachments.size());
	if(!attachments[aid] || !attachments[aid]->img) {
		imGuiText("Image or View were destroyed");
		return;
	}

	auto& view = *attachments[aid];
	auto& img = *view.img;

	refButton(*gui_, view);
	refButton(*gui_, img);

	if(state_) {
		if(state_->attachmentCopy.image) {
			displayImage(draw, state_->attachmentCopy);
		} else {
			imGuiText("Error: {}", state_->errorMessage);
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

		auto flags = ImGuiTableFlags_BordersInner |
			ImGuiTableFlags_Resizable |
			ImGuiTableFlags_SizingStretchProp;
		if(ImGui::BeginTable("Values", 2u, flags)) {
			ImGui::TableSetupColumn(nullptr, 0, 0.25f);
			ImGui::TableSetupColumn(nullptr, 0, 0.75f);

			display(compiled, pcr.type_id, name.c_str(), cmd->boundPushConstants().data);
			ImGui::EndTable();
		}

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

	auto refSrcDst = [&](auto* ccmd) {
		if(!ccmd) {
			return;
		}

		if(view_ == IOView::transferSrc) {
			refButtonD(*gui_, ccmd->src);
		} else  {
			refButtonD(*gui_, ccmd->dst);
		}
	};

	auto refDst = [&](auto* ccmd) {
		if(!ccmd) {
			return;
		}

		dlg_assert(view_ == IOView::transferDst);
		refButtonD(*gui_, ccmd->dst);
	};

	auto refSrc = [&](auto* ccmd) {
		if(!ccmd) {
			return;
		}

		dlg_assert(view_ == IOView::transferSrc);
		refButtonD(*gui_, ccmd->src);
	};

	refSrcDst(dynamic_cast<const CopyImageCmd*>(&cmd));
	refDst(dynamic_cast<const CopyBufferToImageCmd*>(&cmd));
	refSrc(dynamic_cast<const CopyImageToBufferCmd*>(&cmd));
	refSrcDst(dynamic_cast<const BlitImageCmd*>(&cmd));
	refSrcDst(dynamic_cast<const ResolveImageCmd*>(&cmd));

	// addSrcDst(dynamic_cast<const CopyBufferCmd*>(&cmd));
	// addDst(dynamic_cast<const UpdateBufferCmd*>(&cmd));
	// addDst(dynamic_cast<const FillBufferCmd*>(&cmd));
	refDst(dynamic_cast<const ClearColorImageCmd*>(&cmd));
	refDst(dynamic_cast<const ClearDepthStencilImageCmd*>(&cmd));

	// TODO: add support for selecting one of the multiple
	//   cleared attachments
	// if(auto* ccmd = dynamic_cast<const ClearAttachmentCmd*>(&cmd); ccmd) {
	// 	// TODO
	// 	(void) ccmd;
	// }

	if(state_->transferImgCopy.image) {
		displayImage(draw, state_->transferImgCopy);
	} else {
		imGuiText("Error: {}", state_->errorMessage);
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
		} else if(auto* dcmd = dynamic_cast<const DrawIndirectCountCmd*>(command_); dcmd) {
			auto cmdSize = dcmd->indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto count = state_->indirectCommandCount;
			span = span.subspan(4, count * cmdSize); // skip the u32 count
			displayMultidraw(count, dcmd->indexed, span);
		}
	}

	command_->displayInspector(*gui_);
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

	auto drawIndexedCmd = dynamic_cast<const DrawIndexedCmd*>(command_);
	auto drawIndirectCmd = dynamic_cast<const DrawIndirectCmd*>(command_);
	auto dispatchIndirectCmd = dynamic_cast<const DispatchIndirectCmd*>(command_);
	auto drawIndirectCountCmd = dynamic_cast<const DrawIndirectCountCmd*>(command_);

	auto indirectCmd = dispatchIndirectCmd || drawIndirectCmd || drawIndirectCountCmd;
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
			hook.copyAttachment = {viewData_.attachment.id, beforeCommand_};
			break;
		case IOView::transferSrc:
			hook.copyTransferSrc = true;
			hook.copyTransferBefore = beforeCommand_;
			break;
		case IOView::transferDst:
			hook.copyTransferDst = true;
			hook.copyTransferBefore = beforeCommand_;
			break;
		case IOView::ds:
			hook.copyDS = {viewData_.ds.set, viewData_.ds.binding, viewData_.ds.elem, beforeCommand_};
			break;
		case IOView::mesh:
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
	}
}

void CommandViewer::displayImage(Draw& draw, const CopiedImage& img) {
	auto& dev = gui_->dev();

	dlg_assert(img.aspectMask);
	dlg_assert(img.image);
	dlg_assert(img.imageView);

	draw.usedHookState = state_;
	dlg_assert(draw.usedHookState);

	// TODO: when a new CopiedImage is displayed we could reset the
	//   color mask flags. In some cases this is desired but probably
	//   not in all.

	vil::displayImage(*gui_, ioImage_, img.extent, minImageType(img.extent),
		img.format, img.srcSubresRange, nullptr, {});

	VkDescriptorImageInfo dsii {};
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = (ioImage_.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? img.stencilView : img.imageView;
	dsii.sampler = dev.renderData->nearestSampler;

	VkWriteDescriptorSet write {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = draw.dsSelected;
	write.pImageInfo = &dsii;

	dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);
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
