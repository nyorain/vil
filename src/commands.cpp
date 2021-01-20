#include <commands.hpp>
#include <handles.hpp>
#include <shader.hpp>
#include <cb.hpp>
#include <util/span.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/commandHook.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <spirv_reflect.h>
#include <vk/format_utils.h>
#include <vk/enumString.hpp>
#include <iomanip>

namespace fuen {

// Command utility
template<typename C>
auto rawHandles(const C& handles) {
	using VkH = decltype(handle(*handles[0]));
	std::vector<VkH> ret;
	ret.reserve(handles.size());
	for(auto* h : handles) {
		ret.push_back(handle(*h));
	}

	return ret;
}

template<typename H>
void checkUnset(H*& handlePtr, const std::unordered_set<DeviceHandle*>& destroyed) {
	if(handlePtr&& destroyed.count(handlePtr)) {
		handlePtr = nullptr;
	}
}

template<typename H>
void checkUnset(span<H*> handlePtr, const std::unordered_set<DeviceHandle*>& destroyed) {
	for(auto& ptr : handlePtr) {
		checkUnset(ptr, destroyed);
	}
}

NameResult name(DeviceHandle* handle, NullName nullName) {
	if(!handle) {
		switch(nullName) {
			case NullName::null: return {NameType::null, "<null>"};
			case NullName::destroyed: return {NameType::null, "<destroyed>"};
			case NullName::empty: return {NameType::null, ""};

		}
	}

	auto name = fuen::name(*handle);
	if(handle->name.empty()) {
		return {NameType::unnamed, name};
	}

	return {NameType::named, name};
}

std::vector<std::string> createArgumentsDesc() {
	return {};
}

void addToArgumentsDesc(std::vector<std::string>& ret, const char* str) {
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const std::string& str) {
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundVertexBuffer& buf) {
	if(!buf.buffer) {
		return;
	}

	auto str = dlg::format("v {}[{}]", name(buf.buffer).name, buf.offset);
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundIndexBuffer& buf) {
	if(!buf.buffer) {
		return;
	}

	auto str = dlg::format("{}[{}]", name(buf.buffer).name, buf.offset);
	ret.push_back(str);
	ret.push_back(vk::name(buf.type));
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundDescriptorSet& set) {
	if(!set.ds) {
		return;
	}

	ret.push_back(name(set.ds).name);
}

std::string formatScalar(SpvReflectTypeFlags type,
		const SpvReflectNumericTraits& traits, span<const std::byte> data) {
	// TODO
	// - support non-32-bit scalars
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

	dlg_warn("Unsupported scalar type");
	return "";
}

void display(SpvReflectBlockVariable& bvar, span<const std::byte> data);

void displayNonArray(SpvReflectBlockVariable& bvar, span<const std::byte> data) {
	auto& type = nonNull(bvar.type_description);
	data = data.subspan(bvar.offset);
	auto varName = bvar.name ? bvar.name : "?";

	auto scalarFlags =
		SPV_REFLECT_TYPE_FLAG_BOOL |
		SPV_REFLECT_TYPE_FLAG_FLOAT |
		SPV_REFLECT_TYPE_FLAG_INT;
	if((type.type_flags & ~scalarFlags) == 0) { // must be scalar
		auto val = formatScalar(type.type_flags, type.traits.numeric, data.first(bvar.size));

		ImGui::Columns(2);
		imGuiText("{}:", varName);

		ImGui::NextColumn();
		imGuiText("{}", val);
		ImGui::Columns();
	} else if((type.type_flags & ~(scalarFlags | SPV_REFLECT_TYPE_FLAG_VECTOR)) == 0) {
		auto comps = type.traits.numeric.vector.component_count;
		auto* sep = "";
		auto compSize = type.traits.numeric.scalar.width / 8;
		auto varStr = std::string("");
		auto scalarType = type.type_flags & scalarFlags;

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

	} else if((type.type_flags & ~(scalarFlags |
			SPV_REFLECT_TYPE_FLAG_MATRIX | SPV_REFLECT_TYPE_FLAG_VECTOR)) == 0) {
		auto& mt = type.traits.numeric.matrix;
		auto compSize = type.traits.numeric.scalar.width / 8;
		auto scalarType = type.type_flags & scalarFlags;
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
	} else if(type.type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT) {
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
				// TODO: needs spirv reflect support, see issue there
				imGuiText("{}: TODO: runtime array not supported", varName);
			} else {
				for(auto i = 0u; i < at.dims[0]; ++i) {
					auto d = data.subspan(i * at.stride);

					imGuiText("[{}]", i);

					ImGui::Indent();
					displayNonArray(bvar, d);
					ImGui::Unindent();
				}
			}
		}
	} else {
		displayNonArray(bvar, data);
	}
}

template<typename T>
std::string readFormat(u32 count, span<const std::byte> src) {
	auto ret = std::string {};
	auto sep = "";
	for(auto i = 0u; i < count; ++i) {
		ret += dlg::format("{}{}", sep, read<T>(src));
		sep = ", ";
	}

	dlg_assert(src.empty());
	return ret;
}

template<typename T>
std::string readFormatNorm(u32 count, span<const std::byte> src, float mult,
		float clampMin, float clampMax) {
	auto ret = std::string {};
	auto sep = "";
	for(auto i = 0u; i < count; ++i) {
		auto val = std::clamp(read<T>(src) * mult, clampMin, clampMax);
		ret += dlg::format("{}{}", sep, val);
		sep = ", ";
	}

	dlg_assert(src.empty());
	return ret;
}

// TODO: support compresssed formats!
// TODO: we only use this for vertex input. Does rgb/bgr order matter?
//   in that case we need to seriously rework this, more something
//   like the format read function in util/util.hpp
std::string readFormat(VkFormat format, span<const std::byte> src) {
	u32 numChannels = FormatChannelCount(format);
	u32 componentSize = FormatElementSize(format) / numChannels;

	if(FormatIsFloat(format)) {
		switch(componentSize) {
			case 2: return readFormat<f16>(numChannels, src);
			case 4: return readFormat<float>(numChannels, src);
			case 8: return readFormat<double>(numChannels, src);
			default: break;
		}
	} else if(FormatIsUInt(format) || FormatIsUScaled(format)) {
		switch(componentSize) {
			case 1: return readFormat<u8>(numChannels, src);
			case 2: return readFormat<u16>(numChannels, src);
			case 4: return readFormat<u32>(numChannels, src);
			case 8: return readFormat<u64>(numChannels, src);
			default: break;
		}
	} else if(FormatIsInt(format) || FormatIsSScaled(format)) {
		switch(componentSize) {
			case 1: return readFormat<i8>(numChannels, src);
			case 2: return readFormat<i16>(numChannels, src);
			case 4: return readFormat<i32>(numChannels, src);
			case 8: return readFormat<i64>(numChannels, src);
			default: break;
		}
	} else if(FormatIsUNorm(format)) {
		switch(componentSize) {
			case 1: return readFormatNorm<u8> (numChannels, src, 1 / 255.f, 0.f, 1.f);
			case 2: return readFormatNorm<u16>(numChannels, src, 1 / 65536.f, 0.f, 1.f);
			default: break;
		}
	} else if(FormatIsSNorm(format)) {
		switch(componentSize) {
			case 1: return readFormatNorm<i8> (numChannels, src, 1 / 127.f, -1.f, 1.f);
			case 2: return readFormatNorm<i16>(numChannels, src, 1 / 32767.f, -1.f, 1.f);
			default: break;
		}
	} else if(format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) {
		auto rgb = e5b9g9r9ToRgb(read<u32>(src));
		return dlg::format("{}", rgb[0], rgb[1], rgb[2]);
	}

	// TODO: a lot of formats not supported yet!

	dlg_warn("Format {} not supported", vk::name(format));
	return "<Unsupported format>";
}

void displayDs(Gui& gui, const Command& cmd) {
	dlg_assert(gui.cbGui().hook_);
	auto& hook = *gui.cbGui().hook_;
	if(!hook.state) {
		ImGui::Text("Waiting for a submission...");
		return;
	}

	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	if((dispatchCmd && !dispatchCmd->state.pipe) || (drawCmd && !drawCmd->state.pipe)) {
		ImGui::Text("Pipeline was destroyed, can't interpret content");
		return;
	}

	dlg_assert(hook.copyDS);
	auto [setID, bindingID, elemID] = *hook.copyDS;

	auto dss = dispatchCmd ? dispatchCmd->state.descriptorSets : drawCmd->state.descriptorSets;
	if(setID >= dss.size()) {
		ImGui::Text("Set not bound");
		dlg_warn("Set not bound? Shouldn't happen");
		return;
	}

	auto* set = dss[setID].ds;
	if(!set) {
		ImGui::Text("Set was destroyed/invalidated");
		return;
	}

	if(bindingID >= set->bindings.size()) {
		ImGui::Text("Binding not bound");
		dlg_warn("Binding not bound? Shouldn't happen");
		return;
	}

	auto& binding = set->bindings[bindingID];
	if(elemID >= binding.size()) {
		ImGui::Text("Element not bound");
		dlg_warn("Element not bound? Shouldn't happen");
		return;
	}

	auto& elem = binding[elemID];
	if(!elem.valid) {
		ImGui::Text("Binding element not valid");
		// NOTE: i guess this can happen with descriptor indexing
		// dlg_warn("Binding element not valid? Shouldn't happen");
		return;
	}

	dlg_assert(bindingID < set->layout->bindings.size());
	auto bindingLayout = set->layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	auto dsCat = category(dsType);

	auto& dsc = hook.state->dsCopy;

	// == Buffer ==
	if(dsCat == DescriptorCategory::buffer) {
		auto* buf = std::get_if<CopiedBuffer>(&dsc);
		if(!buf) {
			dlg_assert(dsc.index() == 0);
			imGuiText("Error: {}", hook.state->errorMessage);
			return;
		}

		// general info
		auto& srcBuf = nonNull(elem.bufferInfo.buffer);
		refButton(gui, srcBuf);
		ImGui::SameLine();
		drawOffsetSize(elem.bufferInfo);

		// interpret content
		if(dispatchCmd) {
			auto& pipe = nonNull(dispatchCmd->state.pipe);
			auto& refl = nonNull(nonNull(pipe.stage.spirv).reflection);
			if(setID < refl.descriptor_set_count) {
				auto& set = refl.descriptor_sets[setID];
				if(bindingID < set.binding_count) {
					auto& binding = *set.bindings[bindingID];
					auto* ptr = buf->copy.get();
					display(binding.block, {ptr, buf->buffer.size});
				} else {
					ImGui::Text("Binding not used in pipeline");
				}
			} else {
				ImGui::Text("Binding not used in pipeline");
			}
		} else {
			dlg_assert(drawCmd);
			SpvReflectBlockVariable* bestVar = nullptr;

			// In all graphics pipeline stages, find the block with
			// that covers the most of the buffer
			// NOTE: could add explicit dropdown, selecting the
			// stage to view.
			for(auto& stage : drawCmd->state.pipe->stages) {
				auto& refl = nonNull(nonNull(stage.spirv).reflection);
				if(setID < refl.descriptor_set_count) {
					auto& set = refl.descriptor_sets[setID];
					if(bindingID < set.binding_count) {
						auto& binding = *set.bindings[bindingID];
						if(binding.block.type_description && (
								!bestVar || binding.block.size > bestVar->size)) {
							bestVar = &binding.block;
						}
					}
				}
			}

			if(bestVar) {
				auto* ptr = buf->copy.get();
				display(*bestVar, {ptr, buf->buffer.size});
			} else {
				ImGui::Text("Binding not used in pipeline");
			}
		}
	}

	// == Sampler ==
	if(needsSampler(dsType)) {
		if(bindingLayout.pImmutableSamplers) {
			// TODO: display all samplers?
			auto& vksampler = bindingLayout.pImmutableSamplers[0];
			auto& sampler = gui.dev().samplers.getLocked(vksampler);
			refButton(gui, sampler);
		} else {
			refButtonD(gui, elem.imageInfo.sampler);
		}
	}

	// == Image ==
	if(needsImageView(dsType)) {
		auto* img = std::get_if<CopiedImage>(&dsc);
		if(!img) {
			dlg_assert(dsc.index() == 0);
			imGuiText("Error: {}", hook.state->errorMessage);
			return;
		}

		// TODO: display additional information, proper image viewer
		//   but also link original image/imageView
		gui.cbGui().displayImage(*img);
	}

	// == BufferView ==
	// TODO
}

// If it returns true, should display own command stuff in second window
bool displayActionInspector(Gui& gui, const Command& cmd) {
	if(!gui.cbGui().hook_) {
		return true;
	}

	ImGui::Columns(2);
	if(gui.cbGui().columnWidth1_) {
		ImGui::SetColumnWidth(-1, 200.f);
		gui.cbGui().columnWidth1_ = true;
	}

	ImGui::BeginChild("Command IO list");

	auto& hook = *gui.cbGui().hook_;

	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	dlg_assert(dispatchCmd || drawCmd);

	if(ImGui::Selectable("Command")) {
		hook.unsetHookOps();
	}

	// TODO: this seems to need special treatment for arrays?
	// debug with tkn/deferred gbuf pass
	auto modBindingName = [&](const SpvReflectShaderModule& refl, u32 setID, u32 bindingID) -> const char* {
		for(auto s = 0u; s < refl.descriptor_set_count; ++s) {
			auto& set = refl.descriptor_sets[s];
			if(set.set != setID) {
				continue;
			}

			for(auto b = 0u; b < set.binding_count; ++b) {
				auto& binding = *set.bindings[b];
				if (binding.binding != bindingID) {
					continue;
				}

				if(binding.name && *binding.name != '\0') {
					return binding.name;
				} else if(binding.type_description &&
						binding.type_description->type_name &&
						*binding.type_description->type_name != '\0') {
					return binding.type_description->type_name;
				}
			}
		}

		return nullptr;
	};

	auto bindingName = [&](u32 setID, u32 bindingID) -> std::string {
		if(dispatchCmd && dispatchCmd->state.pipe) {
			auto& refl = nonNull(nonNull(dispatchCmd->state.pipe->stage.spirv).reflection);
			if(auto name = modBindingName(refl, setID, bindingID); name) {
				return std::string(name);
			}
		} else if(drawCmd && drawCmd->state.pipe) {
			for(auto& stage : drawCmd->state.pipe->stages) {
				auto& refl = nonNull(nonNull(stage.spirv).reflection);
				if(auto name = modBindingName(refl, setID, bindingID); name) {
					return std::string(name);
				}
			}
		}

		// We come here if no shader has a valid name for the reosurce.
		// TODO: explicitly detect and mark (hide) unused bindings here?
		// No point in displaying them, really.
		// Maybe add option (checkbox or something) for whether to show
		// hidden bindings.
		// BUT TAKE CARE TO ONLY DO IT FOR REALLY UNUSED HANDLES AND
		// NOT JUST BECAUSE SPIRV REFLECT HAS NO NAME FOR US! seperate
		// the two cases.
		// TODO: could try name of bound resource if we really want to show it.
		// TODO: additionally indicate type? Just add a small UBO, SSBO, Image,
		//   Sampler, StorageImage etc prefix?
		// TODO: Previews would be the best on the long run but hard to get
		//   right I guess (also: preview of buffers?)
		return dlg::format("Binding {}", bindingID);
	};

	auto dss = dispatchCmd ? dispatchCmd->state.descriptorSets : drawCmd->state.descriptorSets;
	ImGui::Text("Descriptors");
	for(auto i = 0u; i < dss.size(); ++i) {
		auto& ds = dss[i];
		if(!ds.ds) {
			// TODO: don't display them in first place? especially not
			// when it's only the leftovers?
			auto label = dlg::format("Descriptor Set {}: null", i);
			auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			ImGui::TreeNodeEx(label.c_str(), flags);
			continue;
		}

		auto label = dlg::format("Descriptor Set {}", i);
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		if(ImGui::TreeNode(label.c_str())) {
			for(auto b = 0u; b < ds.ds->bindings.size(); ++b) {
				// TODO: support & display descriptor array
				auto label = bindingName(i, b);
				auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if(hook.copyDS && hook.copyDS->set == i && hook.copyDS->binding == b) {
					flags |= ImGuiTreeNodeFlags_Selected;
				}

				ImGui::TreeNodeEx(label.c_str(), flags);
				if(ImGui::IsItemClicked()) {
					hook.unsetHookOps();
					hook.copyDS = {i, b, 0};
				}
			}

			ImGui::TreePop();
		}
	}

	if(drawCmd) {
		ImGui::Text("Attachments");

		const BeginRenderPassCmd* rpCmd = nullptr;
		for(auto* cmdi : gui.cbGui().command_) {
			if(rpCmd = dynamic_cast<const BeginRenderPassCmd*>(cmdi); rpCmd) {
				break;
			}
		}

		dlg_assert(rpCmd);
		if(rpCmd && rpCmd->rp) {
			auto& desc = nonNull(nonNull(rpCmd->rp).desc);
			auto subpassID = rpCmd->subpassOfDescendant(*gui.cbGui().command_.back());
			auto& subpass = desc.subpasses[subpassID];

			auto addAttachment = [&](auto label, auto id) {
				auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if(hook.copyAttachment && *hook.copyAttachment == id) {
					flags |= ImGuiTreeNodeFlags_Selected;
				}

				ImGui::TreeNodeEx(label.c_str(), flags);
				if(ImGui::IsItemClicked()) {
					hook.unsetHookOps();
					hook.copyAttachment = id;
				}
			};

			// TODO: name them if possible. Could use names in (fragment) shader.
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
		}
	}

	if(drawCmd) {
		auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if(hook.copyAttachment && hook.copyVertexBuffers) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		ImGui::TreeNodeEx("Vertex input", flags);
		if(ImGui::IsItemClicked()) {
			hook.unsetHookOps();
			hook.copyVertexBuffers = true;

			auto indexedCmd = dynamic_cast<const DrawIndexedCmd*>(drawCmd);
			auto indirectCmd = dynamic_cast<const DrawIndirectCmd*>(drawCmd);
			auto indirectCountCmd = dynamic_cast<const DrawIndirectCountCmd*>(drawCmd);
			if(indexedCmd ||
					(indirectCmd && indirectCmd->indexed) ||
					(indirectCountCmd && indirectCountCmd->indexed)) {
				hook.copyIndexBuffers = true;
			}
		}
	}

	// display push constants
	if(dispatchCmd && dispatchCmd->state.pipe) {
		auto& refl = nonNull(nonNull(nonNull(dispatchCmd->state.pipe).stage.spirv).reflection);
		if(refl.push_constant_block_count) {
			auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if(hook.pcr == VK_SHADER_STAGE_COMPUTE_BIT) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			ImGui::TreeNodeEx("Push Constants", flags);
			if(ImGui::IsItemClicked()) {
				hook.unsetHookOps();
				hook.pcr = VK_SHADER_STAGE_COMPUTE_BIT;
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
			if(hook.pcr == stage.stage) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			auto stageName = vk::name(stage.stage);
			auto label = dlg::format("Push Constants {}", stageName);
			ImGui::TreeNodeEx(label.c_str(), flags);
			if(ImGui::IsItemClicked()) {
				hook.unsetHookOps();
				hook.pcr = stage.stage;
			}
		}
	}

	ImGui::EndChild();
	ImGui::NextColumn();
	ImGui::BeginChild("Command IO Inspector");

	// TODO: display more information, not just raw data
	//   e.g. link to the respective resources, descriptor sets etc
	auto cmdInfo = true;
	if(hook.copyDS) {
		displayDs(gui, cmd);
		cmdInfo = false;
	} else if(hook.copyVertexBuffers) {
		dlg_assert(drawCmd);
		if(!drawCmd || !drawCmd->state.pipe) {
			ImGui::Text("Pipeline was destroyed, can't interpret state");
		} else if(!hook.state) {
			ImGui::Text("Waiting for a submission...");
		} else if(hook.state->vertexBufCopies.size() < drawCmd->state.pipe->vertexBindings.size()) {
			if(!hook.state->errorMessage.empty()) {
				imGuiText("Error: {}", hook.state->errorMessage);
			} else {
				ImGui::Text("Error: not enough vertex buffers bound");
			}
		} else {
			// TODO: display binding information
			// TODO: how to display indices?
			// TODO: only show vertex range used for draw call

			auto& pipe = *drawCmd->state.pipe;

			SpvReflectShaderModule* vertStage = nullptr;
			for(auto& stage : pipe.stages) {
				if(stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
					vertStage = &nonNull(nonNull(stage.spirv).reflection);
					break;
				}
			}

			if(!vertStage) {
				ImGui::Text("Grahpics Pipeline has no vertex stage :o");
			} else {
				auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable;

				// match bindings to input variables into
				// (pipe.vertexAttrib, vertStage->input_variables) id pairs
				std::vector<std::pair<u32, u32>> attribs;
				for(auto a = 0u; a < pipe.vertexAttribs.size(); ++a) {
					auto& attrib = pipe.vertexAttribs[a];
					for(auto i = 0u; i < vertStage->input_variable_count; ++i) {
						auto& iv = *vertStage->input_variables[i];
						if(iv.location == attrib.location) {
							attribs.push_back({a, i});
						}
					}
				}

				// TODO sort by input location?

				if(attribs.empty()) {
					ImGui::Text("No Vertex input");
				} else if(ImGui::BeginTable("Vertices", int(attribs.size()), flags)) {
					for(auto& attrib : attribs) {
						ImGui::NextColumn();
						auto& iv = *vertStage->input_variables[attrib.second];
						ImGui::TableSetupColumn(iv.name);
					}

					ImGui::TableHeadersRow();
					ImGui::TableNextRow();

					auto finished = false;
					auto id = 0u;
					while(!finished) {
						for(auto& [aID, _] : attribs) {
							auto& attrib = pipe.vertexAttribs[aID];
							ImGui::TableNextColumn();

							auto& binding = pipe.vertexBindings[attrib.binding];
							auto& buf = hook.state->vertexBufCopies[attrib.binding];
							auto off = binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX ?
								id * binding.stride : 0u;
							off += attrib.offset;

							// TODO: compressed support?
							auto size = FormatElementSize(attrib.format);

							if(off + size > buf.buffer.size) {
								finished = true;
								break;
							}

							auto* ptr = buf.copy.get() + off;
							auto str = readFormat(attrib.format, {ptr, size});

							imGuiText("{}", str);
						}

						++id;
						ImGui::TableNextRow();
					}

					ImGui::EndTable();
				}
			}
		}

		cmdInfo = false;
	} else if(hook.copyAttachment) {
		if(hook.state) {
			if(hook.state->attachmentCopy.image) {
				gui.cbGui().displayImage(hook.state->attachmentCopy);
			} else {
				imGuiText("Error: {}", hook.state->errorMessage);
			}
		} else {
			ImGui::Text("Waiting for a submission...");
		}
		cmdInfo = false;
	} else if(hook.pcr) {
		if(dispatchCmd && dispatchCmd->state.pipe && hook.pcr == dispatchCmd->state.pipe->stage.stage) {
			auto& refl = nonNull(nonNull(nonNull(dispatchCmd->state.pipe).stage.spirv).reflection);
			dlg_assert(refl.push_constant_block_count);
			if(refl.push_constant_block_count) {
				display(*refl.push_constant_blocks, dispatchCmd->pushConstants.data);
			}

			cmdInfo = false;
		} else if(drawCmd && drawCmd->state.pipe) {
			for(auto& stage : drawCmd->state.pipe->stages) {
				if(stage.stage != hook.pcr) {
					continue;
				}

				auto& refl = nonNull(nonNull(stage.spirv).reflection);
				dlg_assert(refl.push_constant_block_count);
				if(refl.push_constant_block_count) {
					display(*refl.push_constant_blocks, drawCmd->pushConstants.data);
				}

				cmdInfo = false;
				break;
			}
		}
	}

	if(cmdInfo) {
		hook.queryTime = true;
		if(hook.state) {
			auto lastTime = hook.state->neededTime;
			auto displayDiff = lastTime * gui.dev().props.limits.timestampPeriod;

			// auto timeNames = {"ns", "mus", "ms", "s"};
			auto timeNames = {"ms"};

			auto it = timeNames.begin();
			while(displayDiff > 1000.f && (it + 1) != timeNames.end()) {
				++it;
				displayDiff /= 1000.f;

			}

			imGuiText("Needed time: {} {}", displayDiff, *it);
		}
	}

	ImGui::EndChild();

	return cmdInfo;
}

template<typename H> using FuenNameExpr = decltype(std::declval<H>().objectType);
template<typename H> using FuenNameExprPtr = decltype(std::declval<H>()->objectType);
template<typename H> using VkNameExpr = decltype(vk::name(std::declval<H>()));
template<typename H> using ToStringExpr = decltype(std::to_string(std::declval<H>()));

template<typename T, typename = std::enable_if_t<
	validExpression<FuenNameExpr, T> ||
	validExpression<FuenNameExprPtr, T> ||
	validExpression<ToStringExpr, T> ||
	validExpression<VkNameExpr, T>>>
void addToArgumentsDesc(std::vector<std::string>& ret, const T& val) {
	if constexpr(validExpression<FuenNameExpr, T>) {
		ret.push_back(fuen::name(val));
	} else if constexpr(validExpression<FuenNameExprPtr, T>) {
		ret.push_back(val ? fuen::name(*val) : "null");
	} else if constexpr(validExpression<VkNameExpr, T>) {
		ret.push_back(vk::name(val));
	} else if constexpr(validExpression<ToStringExpr, T>) {
		ret.push_back(std::to_string(val));
	} else {
		static_assert(templatize<T>(false), "Invalid type");
	}
}

template<typename T>
void addToArgumentsDesc(std::vector<std::string>& ret, span<T> values) {
	for(auto& val : values) {
		addToArgumentsDesc(ret, val);
	}
}

template<typename T>
void addToArgumentsDesc(std::vector<std::string>& ret, const std::vector<T>& values) {
	for(auto& val : values) {
		addToArgumentsDesc(ret, val);
	}
}

template<typename First, typename... Rest>
std::vector<std::string> createArgumentsDesc(const First& first, const Rest&... rest) {
	auto ret = createArgumentsDesc(rest...);
	addToArgumentsDesc(ret, first);
	return ret;
}

// copy util
std::string printImageOffset(Image* img, const VkOffset3D& offset) {
	if(img && img->ci.imageType == VK_IMAGE_TYPE_1D) {
		return dlg::format("{}", offset.x);
	} else if(img && img->ci.imageType == VK_IMAGE_TYPE_2D) {
		return dlg::format("{}, {}", offset.x, offset.y);
	} else {
		return dlg::format("{}, {}, {}", offset.x, offset.y, offset.z);
	}
}

std::string printImageSubresLayers(Image* img, const VkImageSubresourceLayers& subres) {
	std::string subresStr;
	auto sepStr = "";
	if(!img || img->ci.mipLevels > 1) {
		subresStr = dlg::format("{}mip {}", sepStr, subres.mipLevel);
		sepStr = ", ";
	}

	if(!img || img->ci.arrayLayers > 1) {
		if(subres.layerCount > 1) {
			subresStr = dlg::format("{}layers {}..{}", sepStr,
				subres.baseArrayLayer, subres.baseArrayLayer + subres.layerCount - 1);
		} else {
			subresStr = dlg::format("{}layer {}", sepStr, subres.baseArrayLayer);
		}

		sepStr = ", ";
	}

	return subresStr;
}

std::string printImageRegion(Image* img, const VkOffset3D& offset,
		const VkImageSubresourceLayers& subres) {

	auto offsetStr = printImageOffset(img, offset);
	auto subresStr = printImageSubresLayers(img, subres);

	auto sep = subresStr.empty() ? "" : ", ";
	return dlg::format("({}{}{})", offsetStr, sep, subresStr);
}

std::string printBufferImageCopy(Image* image,
		const VkBufferImageCopy& copy, bool bufferToImage) {
	auto imgString = printImageRegion(image, copy.imageOffset, copy.imageSubresource);

	std::string sizeString;
	if(image && image->ci.imageType == VK_IMAGE_TYPE_1D) {
		sizeString = dlg::format("{}", copy.imageExtent.width);
	} else if(image && image->ci.imageType <= VK_IMAGE_TYPE_2D) {
		sizeString = dlg::format("{} x {}", copy.imageExtent.width,
			copy.imageExtent.height);
	} else {
		sizeString = dlg::format("{} x {} x {}", copy.imageExtent.width,
			copy.imageExtent.height, copy.imageExtent.depth);
	}

	auto bufString = dlg::format("offset {}", copy.bufferOffset);
	if(copy.bufferRowLength || copy.bufferImageHeight) {
		bufString += dlg::format(", rowLength {}, imageHeight {}",
			copy.bufferRowLength, copy.bufferImageHeight);
	}

	if(bufferToImage) {
		return dlg::format("({}) -> {} [{}]", bufString, imgString, sizeString);
	} else {
		return dlg::format("({}) -> {} [{}]", imgString, bufString, sizeString);
	}
}

// API
std::vector<const Command*> displayCommands(const Command* cmd,
		const Command* selected, Command::TypeFlags typeFlags) {
	// TODO: should use imgui list clipper, might have *a lot* of commands here.
	// But first we have to restrict what cmd->display can actually do.
	// Would also have to pre-filter commands for that. And stop at every
	// (expanded) parent command (but it's hard to tell whether they are
	// expanded).
	std::vector<const Command*> ret;
	while(cmd) {
		if((typeFlags & cmd->type())) {
			ImGui::Separator();
			if(auto reti = cmd->display(selected, typeFlags); !reti.empty()) {
				dlg_assert(ret.empty());
				ret = reti;
			}
		}

		cmd = cmd->next;
	}

	return ret;
}


// Command
std::vector<const Command*> Command::display(const Command* sel, TypeFlags typeFlags) const {
	if(!(typeFlags & this->type())) {
		return {};
	}

	int flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if(sel == this) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	auto idStr = dlg::format("{}:{}", nameDesc(), relID);
	ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", toString().c_str());

	std::vector<const Command*> ret;
	if(ImGui::IsItemClicked()) {
		ret = {this};
	}

	return ret;
}

bool Command::isChild(const Command& cmd) const {
	auto* it = children();
	while(it) {
		if(it == &cmd) {
			return true;
		}

		it = it->next;
	}

	return false;
}

bool Command::isDescendant(const Command& cmd) const {
	auto* it = children();
	while(it) {
		if(it == &cmd || it->isDescendant(cmd)) {
			return true;
		}

		it = it->next;
	}

	return false;
}

// WaitEventsCmd
void WaitEventsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkEvents = rawHandles(this->events);
	dev.dispatch.CmdWaitEvents(cb,
		u32(vkEvents.size()), vkEvents.data(),
		this->srcStageMask, this->dstStageMask,
		u32(this->memBarriers.size()), this->memBarriers.data(),
		u32(this->bufBarriers.size()), this->bufBarriers.data(),
		u32(this->imgBarriers.size()), this->imgBarriers.data());

}

// Commands
std::vector<const Command*> ParentCommand::display(const Command* selected,
		TypeFlags typeFlags, const Command* cmd) const {
	int flags = ImGuiTreeNodeFlags_OpenOnArrow;
	if(this == selected) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	std::vector<const Command*> ret {};
	auto idStr = dlg::format("{}:{}", nameDesc(), relID);
	auto open = ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", toString().c_str());
	if(ImGui::IsItemClicked()) {
		// don't select when only clicked on arrow
		if(ImGui::GetMousePos().x > ImGui::GetItemRectMin().x + 30) {
			ret = {this};
		}
	}

	if(open) {
		auto retc = displayCommands(cmd, selected, typeFlags);
		if(!retc.empty()) {
			dlg_assert(ret.empty());
			ret = std::move(retc);
			ret.insert(ret.begin(), this);
		}

		ImGui::TreePop();
	}

	return ret;
}

std::vector<const Command*> ParentCommand::display(const Command* selected,
		TypeFlags typeFlags) const {
	return this->display(selected, typeFlags, children());
}

void BarrierCmdBase::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffers, destroyed);
	checkUnset(images, destroyed);
}

std::vector<std::string> WaitEventsCmd::argumentsDesc() const {
	return createArgumentsDesc(events,
		vk::flagNames(VkPipelineStageFlagBits(srcStageMask)),
		vk::flagNames(VkPipelineStageFlagBits(dstStageMask)));
}

void WaitEventsCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	BarrierCmdBase::unset(destroyed);
	checkUnset(events, destroyed);
}

void BarrierCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPipelineBarrier(cb,
		this->srcStageMask, this->dstStageMask, this->dependencyFlags,
		u32(this->memBarriers.size()), this->memBarriers.data(),
		u32(this->bufBarriers.size()), this->bufBarriers.data(),
		u32(this->imgBarriers.size()), this->imgBarriers.data());

}

std::vector<std::string> BarrierCmd::argumentsDesc() const {
	return createArgumentsDesc(
		vk::flagNames(VkPipelineStageFlagBits(srcStageMask)),
		vk::flagNames(VkPipelineStageFlagBits(dstStageMask)));
}

// BeginRenderPassCmd
unsigned BeginRenderPassCmd::subpassOfDescendant(const Command& cmd) const {
	auto subpass = this->children();
	for(auto i = 0u; subpass; ++i, subpass = subpass->next) {
		if(subpass->isDescendant(cmd)) {
			return i;
		}
	}

	return u32(-1);
}

std::string BeginRenderPassCmd::toString() const {
	auto [fbRes, fbName] = name(fb);
	auto [rpRes, rpName] = name(rp);
	if(fbRes == NameType::named && rpRes == NameType::named) {
		return dlg::format("BeginRenderPass({}, {})", rpName, fbName);
	} else if(rpRes == NameType::named) {
		return dlg::format("BeginRenderPass({})", rpName);
	} else {
		return "BeginRenderPass";
	}
}

void BeginRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(this->subpassBeginInfo.pNext) {
		auto f = dev.dispatch.CmdBeginRenderPass2;
		dlg_assert(f);
		f(cb, &this->info, &this->subpassBeginInfo);
	} else {
		dev.dispatch.CmdBeginRenderPass(cb, &this->info, this->subpassBeginInfo.contents);
	}
}

std::vector<const Command*> BeginRenderPassCmd::display(const Command* selected,
		TypeFlags typeFlags) const {
	auto cmd = this->children_;
	auto first = static_cast<FirstSubpassCmd*>(nullptr);
	if(cmd) {
		// If we only have one subpass, don't give it an extra section
		// to make everything more compact.
		first = dynamic_cast<FirstSubpassCmd*>(cmd);
		dlg_assert(first);
		if(!first->next) {
			cmd = first->children_;
		}
	}

	auto ret = ParentCommand::display(selected, typeFlags, cmd);
	if(ret.size() > 1 && cmd != children_) {
		ret.insert(ret.begin() + 1, first);
	}

	return ret;
}

std::vector<std::string> BeginRenderPassCmd::argumentsDesc() const {
	return createArgumentsDesc(rp, subpassBeginInfo.contents);
}

void BeginRenderPassCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(rp, destroyed);
	checkUnset(fb, destroyed);
	ParentCommand::unset(destroyed);
}

void BeginRenderPassCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, fb);
	refButtonD(gui, rp);
}

void NextSubpassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(this->beginInfo.pNext || this->endInfo.pNext) {
		auto f = dev.dispatch.CmdNextSubpass2;
		f(cb, &this->beginInfo, &this->endInfo);
	} else {
		dev.dispatch.CmdNextSubpass(cb, this->beginInfo.contents);
	}
}

void EndRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(this->endInfo.pNext) {
		auto f = dev.dispatch.CmdEndRenderPass2;
		f(cb, &this->endInfo);
	} else {
		dev.dispatch.CmdEndRenderPass(cb);
	}
}

// DrawCmdBase
DrawCmdBase::DrawCmdBase(CommandBuffer& cb, const GraphicsState& gfxState) {
	state = copy(cb, gfxState);
	// NOTE: only do this when pipe layout matches pcr layout
	pushConstants.data = copySpan(cb, cb.pushConstants().data);
}

void DrawCmdBase::displayGrahpicsState(Gui& gui, bool indices) const {
	if(indices) {
		dlg_assert(state.indices.buffer);
		imGuiText("Index Buffer: ");
		ImGui::SameLine();
		refButtonD(gui, state.indices.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}, Type {}", state.indices.offset, vk::name(state.indices.type));
	}

	refButtonD(gui, state.pipe);

	imGuiText("Vertex buffers");
	for(auto& vertBuf : state.vertices) {
		if(!vertBuf.buffer) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, vertBuf.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}", vertBuf.offset);
	}

	// dynamic state
	if(state.pipe && !state.pipe->dynamicState.empty()) {
		imGuiText("DynamicState");
		ImGui::Indent();

		// viewport
		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_VIEWPORT)) {
			auto count = state.pipe->viewportState.viewportCount;
			dlg_assert(state.dynamic.viewports.size() >= count);
			if(count == 1) {
				auto& vp = state.dynamic.viewports[0];
				imGuiText("Viewport: pos ({}, {}), size ({}, {}), depth [{}, {}]",
					vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
			} else if(count > 1) {
				imGuiText("Viewports");
				for(auto& vp : state.dynamic.viewports.first(count)) {
					ImGui::Bullet();
					imGuiText("pos ({}, {}), size ({}, {}), depth [{}, {}]",
						vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
				}
			}
		}
		// scissor
		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_SCISSOR)) {
			auto count = state.pipe->viewportState.scissorCount;
			dlg_assert(state.dynamic.scissors.size() >= count);
			if(count == 1) {
				auto& sc = state.dynamic.scissors[0];
				imGuiText("Scissor: offset ({}, {}), extent ({} {})",
					sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
			} else if(count > 1) {
				imGuiText("Scissors");
				for(auto& sc : state.dynamic.scissors.first(count)) {
					ImGui::Bullet();
					imGuiText("offset ({} {}), extent ({} {})",
						sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
				}
			}
		}

		// line width
		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_LINE_WIDTH)) {
			imGuiText("Line width: {}", state.dynamic.lineWidth);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_DEPTH_BIAS)) {
			auto& db = state.dynamic.depthBias;
			imGuiText("Depth bias: constant {}, clamp {}, slope {}",
				db.constant, db.clamp, db.slope);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
			auto& bc = state.dynamic.blendConstants;
			imGuiText("Blend Constants: {} {} {} {}",
				bc[0], bc[1], bc[2], bc[3]);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_DEPTH_BOUNDS)) {
			imGuiText("Depth bounds: [{}, {}]",
				state.dynamic.depthBoundsMin, state.dynamic.depthBoundsMax);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
			imGuiText("Stencil compare mask front: {}{}", std::hex,
				state.dynamic.stencilFront.compareMask);
			imGuiText("Stencil compare mask back: {}{}", std::hex,
				state.dynamic.stencilBack.compareMask);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
			imGuiText("Stencil write mask front: {}{}", std::hex,
				state.dynamic.stencilFront.writeMask);
			imGuiText("Stencil write mask back: {}{}", std::hex,
				state.dynamic.stencilBack.writeMask);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
			imGuiText("Stencil reference front: {}{}", std::hex,
				state.dynamic.stencilFront.reference);
			imGuiText("Stencil reference back: {}{}", std::hex,
				state.dynamic.stencilBack.reference);
		}

		ImGui::Unindent();
	} else if(!state.pipe) {
		imGuiText("Can't display relevant dynamic state, pipeline was destroyed");
	} else if(state.pipe->dynamicState.empty()) {
		// imGuiText("No relevant dynamic state");
	}

	imGuiText("Descriptors");
	for(auto& ds : state.descriptorSets) {
		if(!ds.ds) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, ds.ds, "<Invalid>");
		// TODO: dynamic offsets
	}

	// TODO: push constants
}

void DrawCmdBase::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(state.pipe, destroyed);
	checkUnset(state.indices.buffer, destroyed);

	for(auto& verts : state.vertices) {
		checkUnset(verts.buffer, destroyed);
	}

	for(auto& ds : state.descriptorSets) {
		checkUnset(ds.ds, destroyed);
		// ds.layout is kept alive
	}
}

// DrawCmd
void DrawCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

std::vector<std::string> DrawCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.vertices, state.descriptorSets,
		vertexCount, instanceCount, firstInstance, firstVertex);
}

std::string DrawCmd::toString() const {
	return dlg::format("Draw({}, {}, {}, {})",
		vertexCount, instanceCount, firstVertex, firstInstance);
}

void DrawCmd::displayInspector(Gui& gui) const {
	auto drawOwn = displayActionInspector(gui, *this);
	if(drawOwn) {
		ImGui::BeginChild("Command IO Inspector");

		asColumns2({{
			{"vertexCount", "{}", vertexCount},
			{"instanceCount", "{}", instanceCount},
			{"firstVertex", "{}", firstVertex},
			{"firstInstance", "{}", firstInstance},
		}});

		DrawCmdBase::displayGrahpicsState(gui, false);

		ImGui::EndChild();
	}
}

// DrawIndirectCmd
void DrawIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(indexed) {
		dev.dispatch.CmdDrawIndexedIndirect(cb, buffer->handle, offset, drawCount, stride);
	} else {
		dev.dispatch.CmdDrawIndirect(cb, buffer->handle, offset, drawCount, stride);
	}
}

std::vector<std::string> DrawIndirectCmd::argumentsDesc() const {
	auto ret = createArgumentsDesc(buffer, offset, drawCount, stride,
		state.pipe, state.vertices, state.descriptorSets);
	if(indexed) {
		addToArgumentsDesc(ret, state.indices);
	}

	return ret;
}

void DrawIndirectCmd::displayInspector(Gui& gui) const {
	auto drawOwn = displayActionInspector(gui, *this);
	if(drawOwn) {
		ImGui::BeginChild("Command IO Inspector");

		imGuiText("Indirect buffer");
		ImGui::SameLine();
		refButtonD(gui, buffer);
		ImGui::SameLine();
		imGuiText("Offset {}", offset);

		DrawCmdBase::displayGrahpicsState(gui, indexed);

		ImGui::EndChild();
	}
}

void DrawIndirectCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
	DrawCmdBase::unset(destroyed);
}

std::string DrawIndirectCmd::toString() const {
	auto [bufNameRes, bufName] = name(buffer);
	auto cmdName = indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	if(bufNameRes == NameType::named) {
		return dlg::format("{}({}, {})", cmdName, bufName, drawCount);
	} else if(drawCount > 1) {
		return dlg::format("{}(drawCount: {})", cmdName, drawCount);
	} else {
		return cmdName;
	}
}

// DrawIndexedCmd
void DrawIndexedCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

std::vector<std::string> DrawIndexedCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.indices,
		state.vertices, state.descriptorSets,
		indexCount, instanceCount, firstInstance, firstIndex);
}

std::string DrawIndexedCmd::toString() const {
	return dlg::format("DrawIndexed({}, {}, {}, {}, {})",
		indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void DrawIndexedCmd::displayInspector(Gui& gui) const {
	if(displayActionInspector(gui, *this)) {
		asColumns2({{
			{"indexCount", "{}", indexCount},
			{"instanceCount", "{}", instanceCount},
			{"firstIndex", "{}", firstIndex},
			{"vertexOffset", "{}", vertexOffset},
			{"firstInstance", "{}", firstInstance},
		}});

		DrawCmdBase::displayGrahpicsState(gui, true);
	}
}

// DrawIndirectCountCmd
void DrawIndirectCountCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(indexed) {
		auto f = dev.dispatch.CmdDrawIndexedIndirectCount;
		f(cb, buffer->handle, offset, countBuffer->handle, countBufferOffset,
			maxDrawCount, stride);
	} else {
		auto f = dev.dispatch.CmdDrawIndirectCount;
		f(cb, buffer->handle, offset,
			countBuffer->handle, countBufferOffset, maxDrawCount, stride);
	}
}

std::string DrawIndirectCountCmd::toString() const {
	// NOTE: we intentionally don't display any extra information here
	// since that's hard to do inuitively
	return indexed ? "DrawIndexedIndirectCount" : "DrawIndirectCount";
}

std::vector<std::string> DrawIndirectCountCmd::argumentsDesc() const {
	auto ret = createArgumentsDesc(buffer, offset, countBuffer, countBufferOffset,
		maxDrawCount, stride, state.pipe, state.vertices, state.descriptorSets);
	if(indexed) {
		addToArgumentsDesc(ret, state.indices);
	}

	return ret;
}

void DrawIndirectCountCmd::displayInspector(Gui& gui) const {
	// TODO: display effective draw commands
	imGuiText("Indirect buffer:");
	ImGui::SameLine();
	refButtonD(gui, buffer);
	ImGui::SameLine();
	imGuiText("Offset {}, Stride {}", offset, stride);

	imGuiText("Count buffer:");
	ImGui::SameLine();
	refButtonD(gui, countBuffer);
	ImGui::SameLine();
	imGuiText("Offset {}", countBufferOffset);

	DrawCmdBase::displayGrahpicsState(gui, indexed);
}

void DrawIndirectCountCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
	checkUnset(countBuffer, destroyed);
	DrawCmdBase::unset(destroyed);
}

// BindVertexBuffersCmd
void BindVertexBuffersCmd::record(const Device& dev, VkCommandBuffer cb) const {
	std::vector<VkBuffer> vkbuffers;
	std::vector<VkDeviceSize> vkoffsets;
	vkbuffers.reserve(buffers.size());
	vkoffsets.reserve(buffers.size());
	for(auto& b : buffers) {
		vkbuffers.push_back(b.buffer->handle);
		vkoffsets.push_back(b.offset);
	}

	dev.dispatch.CmdBindVertexBuffers(cb, firstBinding,
		u32(vkbuffers.size()), vkbuffers.data(), vkoffsets.data());
}

std::string BindVertexBuffersCmd::toString() const {
	if(buffers.size() == 1) {
		auto [buf0NameRes, buf0Name] = name(buffers[0].buffer);
		if(buf0NameRes == NameType::named) {
			return dlg::format("BindVertexBuffer({}: {})", firstBinding, buf0Name);
		} else {
			return dlg::format("BindVertexBuffer({})", firstBinding);
		}
	} else {
		return dlg::format("BindVertexBuffers({}..{})", firstBinding,
			firstBinding + buffers.size() - 1);
	}
}

void BindVertexBuffersCmd::displayInspector(Gui& gui) const {
	for(auto i = 0u; i < buffers.size(); ++i) {
		ImGui::Bullet();
		imGuiText("{}: ", firstBinding + i);
		ImGui::SameLine();
		refButtonD(gui, buffers[i].buffer);
	}
}

void BindVertexBuffersCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	for(auto& buf : buffers) {
		checkUnset(buf.buffer, destroyed);
	}
}

// BindIndexBufferCmd
void BindIndexBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindIndexBuffer(cb, buffer->handle, offset, indexType);
}

void BindIndexBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
}

// BindDescriptorSetCmd
void BindDescriptorSetCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkds = rawHandles(sets);
	dev.dispatch.CmdBindDescriptorSets(cb, pipeBindPoint, pipeLayout->handle,
		firstSet, u32(vkds.size()), vkds.data(),
		u32(dynamicOffsets.size()), dynamicOffsets.data());
}

std::string BindDescriptorSetCmd::toString() const {
	if(sets.size() == 1) {
		auto [ds0Res, ds0Name] = name(sets[0]);
		if(ds0Res == NameType::named) {
			return dlg::format("BindDescriptorSet({}: {})", firstSet, ds0Name);
		} else {
			return dlg::format("BindDescriptorSet({})", firstSet);
		}
	} else {
		return dlg::format("BindDescriptorSets({}..{})",
			firstSet, firstSet + sets.size() - 1);
	}
}

void BindDescriptorSetCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(sets, destroyed);
}

// DispatchCmdBase
DispatchCmdBase::DispatchCmdBase(CommandBuffer& cb, const ComputeState& compState) {
	state = copy(cb, compState);
	// NOTE: only do this when pipe layout matches pcr layout
	pushConstants.data = copySpan(cb, cb.pushConstants().data);
}

void DispatchCmdBase::displayComputeState(Gui& gui) const {
	refButtonD(gui, state.pipe);

	imGuiText("Descriptors");
	for(auto& ds : state.descriptorSets) {
		if(!ds.ds) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, ds.ds, "<Invalid>");
		// TODO: dynamic offsets
	}

	// TODO: push constants
}

void DispatchCmdBase::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(state.pipe, destroyed);
	for(auto& ds : state.descriptorSets) {
		checkUnset(ds.ds, destroyed);
		// ds.layout kept alive
	}
}

// DispatchCmd
void DispatchCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatch(cb, groupsX, groupsY, groupsZ);
}

std::vector<std::string> DispatchCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.descriptorSets, groupsX, groupsY, groupsZ);
}

std::string DispatchCmd::toString() const {
	return dlg::format("Dispatch({}, {}, {})", groupsX, groupsY, groupsZ);
}

void DispatchCmd::displayInspector(Gui& gui) const {
	auto drawOwn = displayActionInspector(gui, *this);
	if(drawOwn) {
		ImGui::BeginChild("Command IO Inspector");

		imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
		DispatchCmdBase::displayComputeState(gui);

		ImGui::EndChild();
	}
}

// DispatchIndirectCmd
void DispatchIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatchIndirect(cb, buffer->handle, offset);
}

void DispatchIndirectCmd::displayInspector(Gui& gui) const {
	if(displayActionInspector(gui, *this)) {
		refButtonD(gui, buffer);
		DispatchCmdBase::displayComputeState(gui);
	}
}

std::string DispatchIndirectCmd::toString() const {
	auto [bufNameRes, bufName] = name(buffer);
	if(bufNameRes == NameType::named) {
		return dlg::format("DispatchIndirect({})", bufName);
	}

	return "DispatchIndirect";
}

std::vector<std::string> DispatchIndirectCmd::argumentsDesc() const {
	return createArgumentsDesc(buffer, offset, state.pipe, state.descriptorSets);
}

void DispatchIndirectCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
	DispatchCmdBase::unset(destroyed);
}

// DispatchBaseCmd
void DispatchBaseCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto f = dev.dispatch.CmdDispatchBase;
	f(cb, baseGroupX, baseGroupY, baseGroupZ, groupsX, groupsY, groupsZ);
}

std::vector<std::string> DispatchBaseCmd::argumentsDesc() const {
	return createArgumentsDesc(baseGroupX, baseGroupY, baseGroupZ,
		groupsX, groupsY, groupsZ);
}

std::string DispatchBaseCmd::toString() const {
	return dlg::format("DispatchBase({}, {}, {}, {}, {}, {})",
		baseGroupX, baseGroupY, baseGroupZ, groupsX, groupsY, groupsZ);
}

void DispatchBaseCmd::displayInspector(Gui& gui) const {
	if(displayActionInspector(gui, *this)) {
		imGuiText("Base: {} {} {}", baseGroupX, baseGroupY, baseGroupZ);
		imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
		DispatchCmdBase::displayComputeState(gui);
	}
}

// CopyImageCmd
void CopyImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		u32(copies.size()), copies.data());
}

std::string CopyImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyImage({} -> {})", srcName, dstName);
	} else {
		return "CopyImage";
	}
}

void CopyImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

void CopyImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Copies");

	for(auto& copy : copies) {
		auto srcRegion = printImageRegion(src, copy.srcOffset, copy.srcSubresource);
		auto dstRegion = printImageRegion(dst, copy.dstOffset, copy.dstSubresource);

		std::string sizeString;
		if(src && dst && src->ci.imageType == VK_IMAGE_TYPE_1D && dst->ci.imageType == VK_IMAGE_TYPE_1D) {
			sizeString = dlg::format("{}", copy.extent.width);
		} else if(src && dst && src->ci.imageType <= VK_IMAGE_TYPE_2D && dst->ci.imageType <= VK_IMAGE_TYPE_2D) {
			sizeString = dlg::format("{} x {}", copy.extent.width, copy.extent.height);
		} else {
			sizeString = dlg::format("{} x {} x {}", copy.extent.width, copy.extent.height, copy.extent.depth);
		}

		ImGui::Bullet();
		imGuiText("{} -> {} [{}]", srcRegion, dstRegion, sizeString);
	}
}

std::vector<std::string> CopyImageCmd::argumentsDesc() const {
	return createArgumentsDesc(src, dst);
}

// CopyBufferToImageCmd
void CopyBufferToImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBufferToImage(cb, src->handle, dst->handle,
		imgLayout, u32(copies.size()), copies.data());
}

void CopyBufferToImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Copies");

	for(auto& copy : copies) {
		ImGui::Bullet();
		imGuiText("{}", printBufferImageCopy(dst, copy, true));
	}
}

std::string CopyBufferToImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyBufferToImage({} -> {})", srcName, dstName);
	} else {
		return "CopyBufferToImage";
	}
}

void CopyBufferToImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

std::vector<std::string> CopyBufferToImageCmd::argumentsDesc() const {
	return createArgumentsDesc(src, dst);
}

// CopyImageToBufferCmd
void CopyImageToBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImageToBuffer(cb, src->handle, imgLayout, dst->handle,
		u32(copies.size()), copies.data());
}

void CopyImageToBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Copies");

	for(auto& copy : copies) {
		ImGui::Bullet();
		imGuiText("{}", printBufferImageCopy(src, copy, false));
	}
}

std::string CopyImageToBufferCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyImageToBuffer({} -> {})", srcName, dstName);
	} else {
		return "CopyImageToBuffer";
	}
}

void CopyImageToBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

std::vector<std::string> CopyImageToBufferCmd::argumentsDesc() const {
	return createArgumentsDesc(src, dst);
}

// BlitImageCmd
void BlitImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBlitImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		u32(blits.size()), blits.data(), filter);
}

void BlitImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	imGuiText("Filter {}", vk::name(filter));

	ImGui::Spacing();
	imGuiText("Blits");

	for(auto& blit : blits) {
		auto srcSubres = printImageSubresLayers(src, blit.srcSubresource);
		auto src0 = printImageOffset(src, blit.srcOffsets[0]);
		auto src1 = printImageOffset(src, blit.srcOffsets[1]);

		auto dstSubres = printImageSubresLayers(dst, blit.dstSubresource);
		auto dst0 = printImageOffset(dst, blit.dstOffsets[0]);
		auto dst1 = printImageOffset(dst, blit.dstOffsets[1]);

		auto srcSep = srcSubres.empty() ? "" : ": ";
		auto dstSep = dstSubres.empty() ? "" : ": ";

		ImGui::Bullet();
		imGuiText("({}{}({})..({}) -> ({}{}({})..({}))",
			srcSubres, srcSep, src0, src1,
			dstSubres, dstSep, dst0, dst1);
	}
}

std::string BlitImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("BlitImage({} -> {})", srcName, dstName);
	} else {
		return "BlitImage";
	}
}

void BlitImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

std::vector<std::string> BlitImageCmd::argumentsDesc() const {
	return createArgumentsDesc(src, dst);
}

// ResolveImageCmd
void ResolveImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResolveImage(cb, src->handle, srcLayout,
		dst->handle, dstLayout, u32(regions.size()), regions.data());
}

void ResolveImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Regions");

	// Basically same as CopyImageCmd
	for(auto& copy : regions) {
		auto srcRegion = printImageRegion(src, copy.srcOffset, copy.srcSubresource);
		auto dstRegion = printImageRegion(dst, copy.dstOffset, copy.dstSubresource);

		std::string sizeString;
		if(src && dst && src->ci.imageType == VK_IMAGE_TYPE_1D && dst->ci.imageType == VK_IMAGE_TYPE_1D) {
			sizeString = dlg::format("{}", copy.extent.width);
		} else if(src && dst && src->ci.imageType <= VK_IMAGE_TYPE_2D && dst->ci.imageType <= VK_IMAGE_TYPE_2D) {
			sizeString = dlg::format("{} x {}", copy.extent.width, copy.extent.height);
		} else {
			sizeString = dlg::format("{} x {} x {}", copy.extent.width, copy.extent.height, copy.extent.depth);
		}

		ImGui::Bullet();
		imGuiText("{} -> {} [{}]", srcRegion, dstRegion, sizeString);
	}
}

std::string ResolveImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("ResolveImage({} -> {})", srcName, dstName);
	} else {
		return "ResolveImage";
	}
}

void ResolveImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

std::vector<std::string> ResolveImageCmd::argumentsDesc() const {
	return createArgumentsDesc(src, dst);
}

// CopyBufferCmd
void CopyBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBuffer(cb, src->handle, dst->handle,
		u32(regions.size()), regions.data());
}

void CopyBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Regions");

	for(auto& region : regions) {
		ImGui::Bullet();
		imGuiText("offsets {} -> {}, size {}", region.srcOffset, region.dstOffset, region.size);
	}
}

std::string CopyBufferCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyBuffer({} -> {})", srcName, dstName);
	} else {
		return "CopyBuffer";
	}
}

void CopyBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

std::vector<std::string> CopyBufferCmd::argumentsDesc() const {
	return createArgumentsDesc(src, dst);
}

// UpdateBufferCmd
void UpdateBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdUpdateBuffer(cb, dst->handle, offset, data.size(), data.data());
}

void UpdateBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

std::string UpdateBufferCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("UpdateBuffer({})", dstName);
	} else {
		return "UpdateBuffer";
	}
}

void UpdateBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	ImGui::SameLine();
	imGuiText("Offset {}", offset);

	// TODO: display data?
}

std::vector<std::string> UpdateBufferCmd::argumentsDesc() const {
	return createArgumentsDesc(dst, offset, data.size());
}

// FillBufferCmd
void FillBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdFillBuffer(cb, dst->handle, offset, size, data);
}

void FillBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

std::string FillBufferCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("FillBuffer({})", dstName);
	} else {
		return "FillBuffer";
	}
}

void FillBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	ImGui::SameLine();
	imGuiText("Offset {}, Size {}", offset, size);

	imGuiText("Filled with {}{}", std::hex, data);
}

std::vector<std::string> FillBufferCmd::argumentsDesc() const {
	return createArgumentsDesc(dst, offset, size, data);
}

// ClearColorImageCmd
void ClearColorImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearColorImage(cb, dst->handle, imgLayout, &color,
		u32(ranges.size()), ranges.data());
}

std::string ClearColorImageCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("ClearColorImage({})", dstName);
	} else {
		return "ClearColorImage";
	}
}

void ClearColorImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

void ClearColorImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	// TODO: color, layout, ranges
}

std::vector<std::string> ClearColorImageCmd::argumentsDesc() const {
	// including color does not seem like a good idea
	return createArgumentsDesc(dst);
}

// ClearDepthStencilImageCmd
void ClearDepthStencilImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearDepthStencilImage(cb, dst->handle, imgLayout, &value,
		u32(ranges.size()), ranges.data());
}

std::string ClearDepthStencilImageCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("ClearDepthStencilImage({})", dstName);
	} else {
		return "ClearColorImage";
	}
}

void ClearDepthStencilImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

void ClearDepthStencilImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	// TODO: value, layout, ranges
}

std::vector<std::string> ClearDepthStencilImageCmd::argumentsDesc() const {
	// including color does not seem like a good idea
	return createArgumentsDesc(dst);
}

// Clear AttachhmentCmd
void ClearAttachmentCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearAttachments(cb, u32(attachments.size()),
		attachments.data(), u32(rects.size()), rects.data());
}

void ClearAttachmentCmd::displayInspector(Gui& gui) const {
	// TODO: we probably need to refer to used render pass/fb here
	(void) gui;
}

std::vector<std::string> ClearAttachmentCmd::argumentsDesc() const {
	std::vector<std::string> ret;
	for(auto& att : attachments) {
		addToArgumentsDesc(ret, att.colorAttachment);
		addToArgumentsDesc(ret, u32(att.aspectMask));
	}
	return ret;
}

// SetEventCmd
void SetEventCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetEvent(cb, event->handle, stageMask);
}

std::string SetEventCmd::toString() const {
	auto [nameRes, eventName] = name(event);
	if(nameRes == NameType::named) {
		return dlg::format("SetEvent({})", eventName);
	}

	return "SetEvent";
}

void SetEventCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(event, destroyed);
}

void SetEventCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, event);
	imGuiText("Stages: {}", vk::flagNames(VkPipelineStageFlagBits(stageMask)));
}

std::vector<std::string> SetEventCmd::argumentsDesc() const {
	return createArgumentsDesc(event, u32(stageMask));
}

// ResetEventCmd
void ResetEventCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResetEvent(cb, event->handle, stageMask);
}

std::string ResetEventCmd::toString() const {
	auto [nameRes, eventName] = name(event);
	if(nameRes == NameType::named) {
		return dlg::format("ResetEvent({})", eventName);
	}

	return "ResetEvent";
}

void ResetEventCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(event, destroyed);
}

void ResetEventCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, event);
	imGuiText("Stages: {}", vk::flagNames(VkPipelineStageFlagBits(stageMask)));
}

std::vector<std::string> ResetEventCmd::argumentsDesc() const {
	return createArgumentsDesc(event, u32(stageMask));
}

// ExecuteCommandsCmd
void ExecuteCommandsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	std::vector<VkCommandBuffer> vkcbs;
	auto child = children_;
	while(child) {
		auto* echild = dynamic_cast<ExecuteCommandsChildCmd*>(child);
		dlg_assert(echild);
		dlg_assert(echild->record_->cb);
		vkcbs.push_back(echild->record_->cb->handle());

		child = child->next;
	}

	dev.dispatch.CmdExecuteCommands(cb, u32(vkcbs.size()), vkcbs.data());
}

std::vector<const Command*> ExecuteCommandsCmd::display(const Command* selected,
		TypeFlags typeFlags) const {
	auto cmd = this->children_;
	auto first = static_cast<ExecuteCommandsChildCmd*>(nullptr);
	if(cmd) {
		// If we only have one subpass, don't give it an extra section
		// to make everything more compact.
		first = dynamic_cast<ExecuteCommandsChildCmd*>(cmd);
		dlg_assert(first);
		if(!first->next) {
			cmd = first->record_->commands;
		}
	}

	auto ret = ParentCommand::display(selected, typeFlags, cmd);
	if(ret.size() > 1 && cmd != this->children_) {
		ret.insert(ret.begin() + 1, first);
	}

	return ret;
}

std::string ExecuteCommandsChildCmd::toString() const {
	auto [cbRes, cbName] = name(record_->cb);
	if(cbRes == NameType::named) {
		return dlg::format("{}: {}", id_, cbName);
	} else {
		return dlg::format("{}", id_);
	}
}

void ExecuteCommandsCmd::displayInspector(Gui& gui) const {
	auto echild = dynamic_cast<ExecuteCommandsChildCmd*>(children_);
	while(echild) {
		// TODO: could link to command buffer (if still valid/linked)
		auto label = dlg::format("View Recording {}", echild->id_);
		if(ImGui::Button(label.c_str())) {
			gui.selectCommands(IntrusivePtr<CommandRecord>(echild->record_), false);
		}

		echild = dynamic_cast<ExecuteCommandsChildCmd*>(echild->next);
	}
}

// BeginDebugUtilsLabelCmd
void BeginDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	VkDebugUtilsLabelEXT label {};
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = this->name;
	std::memcpy(&label.color, this->color.data(), sizeof(label.color));
	dev.dispatch.CmdBeginDebugUtilsLabelEXT(cb, &label);
}

// EndDebugUtilsLabelCmd
void EndDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndDebugUtilsLabelEXT(cb);
}

// BindPipelineCmd
void BindPipelineCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindPipeline(cb, bindPoint, pipe->handle);
}

void BindPipelineCmd::displayInspector(Gui& gui) const {
	dlg_assert(pipe->type == bindPoint);
	if(bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		refButtonD(gui, static_cast<ComputePipeline*>(pipe));
	} else if(bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		refButtonD(gui, static_cast<GraphicsPipeline*>(pipe));
	}
}

std::string BindPipelineCmd::toString() const {
	auto bp = (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) ? "compute" : "graphics";
	auto [nameRes, pipeName] = name(pipe);
	if(nameRes == NameType::named) {
		return dlg::format("BindPipeline({}, {})", bp, pipeName);
	} else {
		return dlg::format("BindPipeline({})", bp);
	}
}

void BindPipelineCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pipe, destroyed);
}

void PushConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPushConstants(cb, pipeLayout->handle, stages, offset,
		u32(values.size()), values.data());
}

void SetViewportCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetViewport(cb, first, u32(viewports.size()), viewports.data());
}

void SetScissorCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetScissor(cb, first, u32(scissors.size()), scissors.data());
}

void SetLineWidthCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetLineWidth(cb, width);
}

void SetDepthBiasCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetDepthBias(cb, state.constant, state.clamp, state.slope);
}

void SetBlendConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetBlendConstants(cb, values.data());
}

void SetStencilCompareMaskCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilCompareMask(cb, faceMask, value);
}

void SetStencilWriteMaskCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilWriteMask(cb, faceMask, value);
}

void SetStencilReferenceCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilReference(cb, faceMask, value);
}

void SetDepthBoundsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetDepthBounds(cb, min, max);
}

// BeginQuery
void BeginQueryCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBeginQuery(cb, pool->handle, query, flags);
}

void BeginQueryCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// EndQuery
void EndQueryCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndQuery(cb, pool->handle, query);
}

void EndQueryCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// ResetQuery
void ResetQueryPoolCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResetQueryPool(cb, pool->handle, first, count);
}

void ResetQueryPoolCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// WriteTimestamp
void WriteTimestampCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdWriteTimestamp(cb, stage, pool->handle, query);
}

void WriteTimestampCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// CopyQueryPool
void CopyQueryPoolResultsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyQueryPoolResults(cb, pool->handle, first, count,
		dstBuffer->handle, dstOffset, stride, flags);
}

void CopyQueryPoolResultsCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
	checkUnset(dstBuffer, destroyed);
}

} // namespace fuen
