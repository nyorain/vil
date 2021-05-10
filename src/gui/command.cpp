#include <gui/command.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <pipe.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <ds.hpp>
#include <shader.hpp>
#include <rp.hpp>
#include <spirv_reflect.h>
#include <tracy/Tracy.hpp>
#include <bitset>

// NOTE: since we might view invalidated command records, we can't assume
//   any handles in Command objects to be not null (they are unset to null
//   when the cooresponding handle is destroyed/invalidated).

namespace vil {

// util
SpvReflectDescriptorBinding* getReflectBinding(const SpvReflectShaderModule& mod,
		unsigned setID, unsigned bindingID) {
	for(auto s = 0u; s < mod.descriptor_set_count; ++s) {
		auto& set = mod.descriptor_sets[s];
		if(set.set != setID) {
			continue;
		}

		for(auto b = 0u; b < set.binding_count; ++b) {
			auto* binding = set.bindings[b];
			if(binding->binding == bindingID) {
				return binding;
			}
		}
	}

	return nullptr;
}

std::string formatScalar(SpvReflectTypeFlags type,
		const SpvReflectNumericTraits& traits, span<const std::byte> data) {
	if(type == SPV_REFLECT_TYPE_FLAG_INT) {
		dlg_assert(traits.scalar.width == 32);
		auto sgn = traits.scalar.signedness;
		switch(traits.scalar.width) {
			case 8:  return dlg::format("{}", sgn ? copy<i8> (data) : copy<u8> (data));
			case 16: return dlg::format("{}", sgn ? copy<i16>(data) : copy<u16>(data));
			case 32: return dlg::format("{}", sgn ? copy<i32>(data) : copy<u32>(data));
			case 64: return dlg::format("{}", sgn ? copy<i64>(data) : copy<u64>(data));
			default: break;
		}
	} else if(type == SPV_REFLECT_TYPE_FLAG_FLOAT) {
		switch(traits.scalar.width) {
			case 16: return dlg::format("{}", copy<f16>(data));
			case 32: return dlg::format("{}", copy<float>(data));
			case 64: return dlg::format("{}", copy<double>(data));
			default: break;
		}
	} else if(type == SPV_REFLECT_TYPE_FLAG_BOOL) {
		switch(traits.scalar.width) {
			case 8: return dlg::format("{}", bool(copy<u8>(data)));
			case 16: return dlg::format("{}", bool(copy<u16>(data)));
			case 32: return dlg::format("{}", bool(copy<u32>(data)));
			case 64: return dlg::format("{}", bool(copy<u64>(data)));
			default: break;
		}
	}

	dlg_warn("Unsupported scalar type (flags {})", std::bitset<32>(u32(type)));
	return "<Unsupported type>";
}

void display(SpvReflectBlockVariable& bvar, span<const std::byte> data);

// TODO: probably cleaner to use a table for display here
void displayNonArray(SpvReflectBlockVariable& bvar, span<const std::byte> data,
		const char* varName) {
	auto& type = nonNull(bvar.type_description);
	data = data.subspan(bvar.offset);
	varName = varName ? varName : (bvar.name ? bvar.name : "?");

	auto typeFlags = type.type_flags & (~SPV_REFLECT_TYPE_FLAG_ARRAY);
	auto scalarFlags =
		SPV_REFLECT_TYPE_FLAG_BOOL |
		SPV_REFLECT_TYPE_FLAG_FLOAT |
		SPV_REFLECT_TYPE_FLAG_INT;
	if((typeFlags & ~scalarFlags) == 0) { // must be scalar
		auto nBytes = bvar.numeric.scalar.width / 8;
		dlg_assert(data.size() >= nBytes);
		auto val = formatScalar(typeFlags, type.traits.numeric, data.first(nBytes));

		ImGui::Columns(2);
		imGuiText("{}:", varName);

		ImGui::NextColumn();
		imGuiText("{}", val);
		ImGui::Columns();
	} else if((typeFlags & ~(scalarFlags | SPV_REFLECT_TYPE_FLAG_VECTOR)) == 0) {
		auto comps = type.traits.numeric.vector.component_count;
		auto* sep = "";
		auto compSize = type.traits.numeric.scalar.width / 8;
		auto varStr = std::string("");
		auto scalarType = typeFlags & scalarFlags;

		for(auto i = 0u; i < comps; ++i) {
			auto d = data.subspan(i * compSize, compSize);
			auto var = formatScalar(scalarType, type.traits.numeric, d);
			varStr += dlg::format("{}{}", sep, var);
			sep = ", ";
		}

		ImGui::Columns(2);
		imGuiText("{}:", varName);

		ImGui::NextColumn();
		imGuiText("{}", varStr);
		ImGui::Columns();

	} else if((typeFlags & ~(scalarFlags |
			SPV_REFLECT_TYPE_FLAG_MATRIX | SPV_REFLECT_TYPE_FLAG_VECTOR)) == 0) {
		auto& mt = type.traits.numeric.matrix;
		auto compSize = type.traits.numeric.scalar.width / 8;
		auto scalarType = typeFlags & scalarFlags;
		auto rowMajor = bvar.decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR;

		auto deco = "";
		if(rowMajor) {
			deco = " [row major memory]";
		} else {
			deco = " [column major memory]";
		}

		ImGui::Columns(2);
		imGuiText("{}{}:", varName, deco);

		ImGui::NextColumn();

		if(ImGui::BeginTable("Matrix", mt.column_count)) {
			for(auto r = 0u; r < mt.row_count; ++r) {
				ImGui::TableNextRow();

				for(auto c = 0u; c < mt.column_count; ++c) {
					auto offset = rowMajor ? r * mt.stride + c * compSize : c * mt.stride + r * compSize;
					auto d = data.subspan(offset, compSize);
					auto var = formatScalar(scalarType, type.traits.numeric, d);
					ImGui::TableNextColumn();
					imGuiText("{}", var);
				}
			}

			ImGui::EndTable();
		}

		ImGui::Columns();
	} else if(typeFlags & SPV_REFLECT_TYPE_FLAG_STRUCT) {
		imGuiText("{}", varName);
	} else {
		imGuiText("{}: TODO not implemented", varName);
	}

	ImGui::Separator();

	for(auto m = 0u; m < bvar.member_count; ++m) {
		auto& member = bvar.members[m];
		ImGui::Indent();
		display(member, data);
		ImGui::Unindent();
	}
}

void display(SpvReflectBlockVariable& bvar, span<const std::byte> data) {
	auto& type = nonNull(bvar.type_description);
	auto varName = bvar.name ? bvar.name : "?";

	// TODO: arrays (but pretty much the whole buffer display thing) could
	// profit from good ImGui-based clipping. E.g. skip all formatting when
	// not visible. And for arrays we can use ListClip i guess
	if(type.type_flags & SPV_REFLECT_TYPE_FLAG_ARRAY) {
		auto& at = type.traits.array;
		if(at.dims_count != 1u) {
			// TODO: fix this
			imGuiText("{}: TODO: multiple array dimensions not supported", varName);
		} else {
			if(at.dims[0] == 0xFFFFFFFF) {
				// TODO: needs spirv reflect support, see issue there
				imGuiText("{}: TODO: specialization constant array size not supported", varName);
			} else if(at.dims[0] == 0u) {
				// runtime array
				// TODO: implement paging
				constexpr auto maxCount = 100;

				auto varName = bvar.name ? bvar.name : "?";
				auto i = 0u;
				while(data.size() >= at.stride && i < maxCount) {
					auto d = data.subspan(0, at.stride);
					auto name = dlg::format("{}[{}]", varName, i++);

					ImGui::Indent();
					displayNonArray(bvar, d, name.c_str());
					ImGui::Unindent();

					data = data.subspan(at.stride);
				}
			} else {
				// TODO: limit somehow, might be huge
				auto varName = bvar.name ? bvar.name : "?";
				for(auto i = 0u; i < at.dims[0]; ++i) {
					auto d = data.subspan(i * at.stride);

					auto name = dlg::format("{}[{}]", varName, i);

					ImGui::Indent();
					displayNonArray(bvar, d, name.c_str());
					ImGui::Unindent();
				}
			}
		}
	} else {
		displayNonArray(bvar, data, nullptr);
	}
}

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
	auto drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	auto dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
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
				dlg_assert(id < drawCmd->state.rpi.rp->desc->attachments.size());
			}
			break;
		} case IOView::ds: {
			if(!drawCmd && !dispatchCmd) {
				selectCommandView = true;
				break;
			}

			auto* lastDrawCmd = dynamic_cast<const DrawCmdBase*>(command_);
			auto* lastDispatchCmd = dynamic_cast<const DispatchCmdBase*>(command_);
			dlg_assert(lastDrawCmd || lastDispatchCmd);

			// when the newly select command uses a descriptor set with different
			// layout at the selected set we reset the selection. We don't
			// compare cmd->state.descriptorSets as that is often invalidated.
			// Using the layout of the bound pipe is more robust.
			PipelineLayout* newPL {};
			if(dispatchCmd && dispatchCmd->state.pipe) {
				newPL = dispatchCmd->state.pipe->layout.get();
			} else if(drawCmd&& drawCmd->state.pipe) {
				newPL = drawCmd->state.pipe->layout.get();
			}

			PipelineLayout* oldPL {};
			if(lastDispatchCmd && lastDispatchCmd->state.pipe) {
				oldPL = lastDispatchCmd->state.pipe->layout.get();
			} else if(lastDrawCmd&& lastDrawCmd->state.pipe) {
				oldPL = lastDrawCmd->state.pipe->layout.get();
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
			// we keep the selection if pcr in the selected stage exists in
			// the new command
			if(dispatchCmd && dispatchCmd->state.pipe) {
				auto& refl = nonNull(nonNull(dispatchCmd->state.pipe->stage.spirv).reflection);
				if(refl.push_constant_block_count == 0 ||
						viewData_.pushConstants.stage != VK_SHADER_STAGE_COMPUTE_BIT) {
					selectCommandView = true;
				}
			} else if(drawCmd && drawCmd->state.pipe) {
				auto& pipe = *drawCmd->state.pipe;
				auto found = false;
				for(auto& stage : pipe.stages) {
					auto& refl = nonNull(stage.spirv->reflection);
					if(refl.push_constant_block_count &&
							viewData_.pushConstants.stage == stage.stage) {
						found = true;
						break;
					}
				}

				if(!found) {
					selectCommandView = true;
				}
			} else {
				selectCommandView = true;
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
	auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf;

	auto addSrc = [&](auto* cmd) {
		if(!cmd) {
			return;
		}

		found = true;

		if(view_ == IOView::transferSrc) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		// TODO: display more information about src
		ImGui::TreeNodeEx("Source", flags);
		if(ImGui::IsItemClicked()) {
			view_ = IOView::transferSrc;
			ioImage_ = {};
			updateHook();
		}

		ImGui::TreePop();
	};

	auto addDst = [&](auto* cmd) {
		if(!cmd) {
			return;
		}

		found = true;

		if(view_ == IOView::transferDst) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		// TODO: display more information about dst
		ImGui::TreeNodeEx("Destination", flags);
		if(ImGui::IsItemClicked()) {
			view_ = IOView::transferDst;
			ioImage_ = {};
			updateHook();
		}

		ImGui::TreePop();
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
	auto& cmd = nonNull(command_);

	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	if(!drawCmd && !dispatchCmd) {
		return;
	}

	// TODO: make this a runtime setting
	static constexpr auto showUnboundSets = true;
	static constexpr auto showUnusedBindings = false;
	static constexpr auto unnamedName = "<unnamed>";

	auto modBindingName = [&](const SpvReflectShaderModule& refl, u32 setID, u32 bindingID) -> const char* {
		auto* binding = getReflectBinding(refl, setID, bindingID);
		if(binding) {
			if(binding->name && binding->name[0] != '\0') {
				return binding->name;
			} else if(binding->type_description &&
					binding->type_description->type_name &&
					binding->type_description->type_name[0] != '\0') {
				return binding->type_description->type_name;
			}

			return unnamedName;
		}

		return nullptr;
	};

	// Returns nullopt if the binding isn't used.
	auto bindingName = [&](u32 setID, u32 bindingID) -> std::optional<const char*> {
		const char* best = nullptr;
		if(dispatchCmd && dispatchCmd->state.pipe) {
			auto& refl = nonNull(nonNull(dispatchCmd->state.pipe->stage.spirv).reflection);
			best = modBindingName(refl, setID, bindingID);
		} else if(drawCmd && drawCmd->state.pipe) {
			for(auto& stage : drawCmd->state.pipe->stages) {
				auto& refl = nonNull(nonNull(stage.spirv).reflection);
				if(auto name = modBindingName(refl, setID, bindingID); name) {
					best = name;
					if(best != unnamedName) {
						return best;
					}
				}
			}
		}

		if(best) {
			return best;
		}

		// We come here if no shader has a valid name for the reosurce.
		return showUnusedBindings ? std::optional("<unused>") : std::nullopt;
	};

	auto dss = dispatchCmd ? dispatchCmd->state.descriptorSets : drawCmd->state.descriptorSets;
	ImGui::Text("Descriptors");

	// NOTE: better to iterate over sets/bindings in shader stages?
	for(auto i = 0u; i < dss.size(); ++i) {
		auto& ds = dss[i];

		if(!ds.ds) {
			if(showUnboundSets) {
				auto label = dlg::format("Descriptor Set {}: null", i);
				auto flags = ImGuiTreeNodeFlags_Bullet |
					ImGuiTreeNodeFlags_Leaf |
					ImGuiTreeNodeFlags_NoTreePushOnOpen;
				ImGui::TreeNodeEx(label.c_str(), flags);
			}

			continue;
		}

		auto stateIt = dsState_.states.find(ds.ds);
		dlg_assert_or(stateIt != dsState_.states.end(), continue);

		auto& state = *stateIt->second;
		dlg_assert(state.layout);

		auto label = dlg::format("Descriptor Set {}", i);
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		if(ImGui::TreeNode(label.c_str())) {
			for(auto b = 0u; b < state.layout->bindings.size(); ++b) {
				auto oLabel = bindingName(i, b);
				if(!oLabel) {
					continue;
				}

				auto label = *oLabel;
				auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if(view_ == IOView::ds && viewData_.ds.set == i && viewData_.ds.binding == b) {
					flags |= ImGuiTreeNodeFlags_Selected;
				}

				// NOTE
				// - additionally indicate type? Just add a small UBO, SSBO, Image,
				//   Sampler, StorageImage etc prefix? Also array bounds
				// - Previews would be the best on the long run but hard to get
				//   right I guess (also: preview of buffers?)
				// - could show name of bound resource
				auto msg = dlg::format("{}: {}", b, label);
				ImGui::TreeNodeEx(msg.c_str(), flags);
				if(ImGui::IsItemClicked()) {
					view_ = IOView::ds;
					viewData_.ds = {i, b, 0};
					ioImage_ = {};
					updateHook();
				}
			}

			ImGui::TreePop();
		}
	}
}

void CommandViewer::displayIOList() {
	auto& cmd = nonNull(command_);

	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);

	if(ImGui::Selectable("Command", view_ == IOView::command)) {
		view_ = IOView::command;
		updateHook();
	}

	// Transfer IO
	if(cmd.type() == CommandType::transfer) {
		displayTransferIOList();
	}

	// Vertex IO
	if(drawCmd) {
		auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
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
		if(ImGui::TreeNodeEx("Attachments")) {
			if(drawCmd->state.rpi.rp) {
				auto& desc = nonNull(drawCmd->state.rpi.rp->desc);
				auto subpassID = drawCmd->state.rpi.subpass;
				dlg_assert(subpassID < desc.subpasses.size());

				auto& subpass = desc.subpasses[subpassID];

				auto addAttachment = [&](auto label, auto id) {
					if(id == VK_ATTACHMENT_UNUSED) {
						return;
					}

					auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
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
					auto label = dlg::format("Depth Stencil Attachment");
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

	if(dispatchCmd && dispatchCmd->state.pipe) {
		auto& refl = nonNull(nonNull(nonNull(dispatchCmd->state.pipe).stage.spirv).reflection);

		if(refl.push_constant_block_count) {
			auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if(viewPCRStage == VK_SHADER_STAGE_COMPUTE_BIT) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			ImGui::TreeNodeEx("Push Constants", flags);
			if(ImGui::IsItemClicked()) {
				view_ = IOView::pushConstants;
				viewData_.pushConstants = {VK_SHADER_STAGE_COMPUTE_BIT};
				updateHook();
			}
		}
	} else if(drawCmd && drawCmd->state.pipe) {
		auto& pipe = nonNull(drawCmd->state.pipe);
		for(auto& stage : pipe.stages) {
			auto& refl = nonNull(nonNull(stage.spirv).reflection);
			if(!refl.push_constant_block_count) {
				continue;
			}

			auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if(viewPCRStage == stage.stage) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			auto stageName = vk::name(stage.stage);
			auto label = dlg::format("Push Constants {}", stageName);
			ImGui::TreeNodeEx(label.c_str(), flags);
			if(ImGui::IsItemClicked()) {
				view_ = IOView::pushConstants;
				viewData_.pushConstants = {stage.stage};
				updateHook();
			}
		}
	}
}

void CommandViewer::displayBeforeCheckbox() {
	if(ImGui::Checkbox("Before Command", &beforeCommand_)) {
		updateHook();
	}
}

void CommandViewer::displayDs(Draw& draw) {
	auto& gui = *this->gui_;
	auto& cmd = nonNull(command_);
	displayBeforeCheckbox();

	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	if(!drawCmd && !dispatchCmd) {
		dlg_error("Unreachable");
		return;
	}

	if((dispatchCmd && !dispatchCmd->state.pipe) || (drawCmd && !drawCmd->state.pipe)) {
		ImGui::Text("Pipeline was destroyed, can't interpret content");
		return;
	}

	dlg_assert(view_ == IOView::ds);
	auto [setID, bindingID, _] = viewData_.ds;

	auto dss = dispatchCmd ? dispatchCmd->state.descriptorSets : drawCmd->state.descriptorSets;
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

	auto bindingCount = descriptorCount(state, bindingID);
	auto& elemID = viewData_.ds.elem;
	if(optSliderRange("Element", elemID, bindingCount)) {
		updateHook();
		state_ = {};
	}

	if(elemID >= bindingCount) {
		ImGui::Text("Element not bound");
		dlg_warn("Element not bound? Shouldn't happen");
		return;
	}

	/*
	auto& elem = bindings[elemID];
	if(!elem.valid) {
		ImGui::Text("Binding element not valid");
		// NOTE: i guess this can happen with descriptor indexing
		// dlg_warn("Binding element not valid? Shouldn't happen");
		return;
	}
	*/

	auto& bindingLayout = state.layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	auto dsCat = category(dsType);

	imGuiText("{}", vk::name(dsType));

	// TODO: we expect descriptors to be valid here. Needs rework
	// for descriptor indexing

	// == Buffer ==
	if(dsCat == DescriptorCategory::buffer) {
		// TODO: take dynamic offset into account for dynamic bufs

		// general info
		auto& elem = buffers(state, bindingID)[elemID];
		auto& srcBuf = nonNull(elem.buffer);
		refButton(gui, srcBuf);
		ImGui::SameLine();
		drawOffsetSize(elem);

		// interpret content
		if(!state_) {
			ImGui::Text("Waiting for a submission...");
			return;
		}

		auto* buf = std::get_if<CopiedBuffer>(&state_->dsCopy);
		if(!buf) {
			dlg_assert(state_->dsCopy.index() == 0);
			imGuiText("Error: {}", state_->errorMessage);
			return;
		}

		if(dispatchCmd) {
			auto& pipe = nonNull(dispatchCmd->state.pipe);
			auto& refl = nonNull(nonNull(pipe.stage.spirv).reflection);
			auto* binding = getReflectBinding(refl, setID, bindingID);
			if(!binding || !binding->block.type_description) {
				ImGui::Text("Binding not used in pipeline");
			} else {
				display(binding->block, buf->copy);
			}
		} else {
			dlg_assert(drawCmd);
			SpvReflectBlockVariable* bestVar = nullptr;

			// In all graphics pipeline stages, find the block with
			// that covers the most of the buffer
			// TODO: add explicit dropdown, selecting the stage to view.
			for(auto& stage : drawCmd->state.pipe->stages) {
				auto& refl = nonNull(nonNull(stage.spirv).reflection);
				auto* binding = getReflectBinding(refl, setID, bindingID);

				if(binding && binding->block.type_description && (
						!bestVar || binding->block.size > bestVar->size)) {
					bestVar = &binding->block;
				}
			}

			if(bestVar) {
				display(*bestVar, buf->copy);
			} else {
				ImGui::Text("Binding not used in pipeline");
			}
		}
	}

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
		refButton(gui, nonNull(imgView.img));

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

	// == BufferView ==
	// TODO
}

void CommandViewer::displayAttachment(Draw& draw) {
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(command_);
	dlg_assert(drawCmd);

	// NOTE: maybe only show this button for output attachments (color, depthStencil)?
	// Does not make sense otherwise as it stays the same i guess.
	// But could be useful for debugging nonetheless
	displayBeforeCheckbox();

	if(!drawCmd->state.rpi.fb) {
		imGuiText("Framebuffer was destroyed");
		return;
	}

	// information
	auto aid = viewData_.attachment.id;
	auto& fb = *drawCmd->state.rpi.fb;
	dlg_assert(aid < fb.attachments.size());
	if(!fb.attachments[aid] || !fb.attachments[aid]->img) {
		imGuiText(" Image or View were destroyed");
		return;
	}

	auto& view = *fb.attachments[aid];
	auto& img = *view.img;

	refButton(*gui_, fb);
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
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(command_);
	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(command_);
	auto viewStage = viewData_.pushConstants.stage;

	if(dispatchCmd) {
		if(!dispatchCmd->state.pipe) {
			ImGui::Text("Pipeline was destroyed, can't interpret content");
			return;
		} else if(viewStage != dispatchCmd->state.pipe->stage.stage) {
			dlg_warn("Invalid push constants stage? Should not happen! (compute)");
			ImGui::Text("Error: Invalid push constants stage selected");
			return;
		}

		auto& refl = nonNull(nonNull(nonNull(dispatchCmd->state.pipe).stage.spirv).reflection);
		dlg_assert(refl.push_constant_block_count == 1);
		if(refl.push_constant_block_count) {
			display(nonNull(refl.push_constant_blocks), dispatchCmd->pushConstants.data);
		}
	} else if(drawCmd && drawCmd->state.pipe) {
		if(!drawCmd->state.pipe) {
			ImGui::Text("Pipeline was destroyed, can't interpret content");
			return;
		}

		auto found = false;
		for(auto& stage : drawCmd->state.pipe->stages) {
			if(stage.stage != viewStage) {
				continue;
			}

			found = true;
			auto& refl = nonNull(nonNull(stage.spirv).reflection);
			dlg_assert(refl.push_constant_block_count);
			if(refl.push_constant_block_count) {
				display(nonNull(refl.push_constant_blocks), drawCmd->pushConstants.data);
			}

			break;
		}

		if(!found) {
			dlg_warn("Invalid push constants stage? Should not happen! (graphics)");
			ImGui::Text("Error: Invalid push constants stage selected");
			return;
		}
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
	displayBeforeCheckbox();

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

	ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, 250.f);
	ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	ImGui::BeginChild("Command IO list");

	displayIOList();

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

	if(state_ && state_->indirectCopy.buffer.size) {
		auto& ic = state_->indirectCopy;
		auto span = ReadBuf(ic.copy);
		if(auto* dcmd = dynamic_cast<const DrawIndirectCmd*>(command_); dcmd) {
			displayMultidraw(dcmd->drawCount, dcmd->indexed, span);
		} else if(dynamic_cast<const DispatchIndirectCmd*>(command_)) {
			auto ecmd = read<VkDispatchIndirectCommand>(span);
			imGuiText("groups X: {}", ecmd.x);
			imGuiText("groups Y: {}", ecmd.y);
			imGuiText("groups Z: {}", ecmd.z);
		} else if(auto* dcmd = dynamic_cast<const DrawIndirectCountCmd*>(command_); dcmd) {
			displayMultidraw(state_->indirectCommandCount, dcmd->indexed, span);
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

} // namespace vil
