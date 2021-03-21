#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <gui/util.hpp>
#include <queue.hpp>
#include <swapchain.hpp>
#include <image.hpp>
#include <rp.hpp>
#include <shader.hpp>
#include <pipe.hpp>
#include <cb.hpp>
#include <commands.hpp>
#include <record.hpp>
#include <util/bytes.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <spirv_reflect.h>
#include <bitset>

namespace vil {

// util
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
		auto val = formatScalar(typeFlags, type.traits.numeric, data.first(bvar.size));

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
// TODO: rgb/bgr order matters here! Fix that.
//   We need to seriously rework this, more something
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


// CommandBufferGui
void CommandBufferGui::init(Gui& gui) {
	gui_ = &gui;

	commandFlags_ = CommandType(~(CommandType::end | CommandType::bind | CommandType::query));
	ioImage_.flags = DrawGuiImage::flagMaskR |
		DrawGuiImage::flagMaskG |
		DrawGuiImage::flagMaskB;

	vertexViewer_.init(gui.dev(), gui.rp());
}

CommandBufferGui::~CommandBufferGui() = default;

void CommandBufferGui::draw(Draw& draw) {
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
			(record_ && record_->group) ||
			(cb_ || record_->cb);

		if(showCombo && ImGui::BeginCombo("Update Source", modeName(mode_))) {
			if(ImGui::Selectable("None")) {
				mode_ = UpdateMode::none;
				hook.target = {};
			}

			if(record_ && record_->group && ImGui::Selectable("CommandGroup")) {
				mode_ = UpdateMode::commandGroup;
				hook.target.group = record_->group;
			}

			if((mode_ == UpdateMode::commandBuffer && cb_) || record_->cb) {
				if(ImGui::Selectable("CommandBuffer")) {
					if(!cb_) {
						cb_ = record_->cb;
					}

					mode_ = UpdateMode::commandBuffer;
					hook.target.cb = cb_;
				}
			}

			ImGui::EndCombo();
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

		if(cb_->lastRecordPtrLocked() != record_) {
			record_ = cb_->lastRecordPtrLocked();
			auto hierarchy = CommandDesc::findHierarchy(record_->commands, desc_);
			command_ = {hierarchy.begin(), hierarchy.end()};
			desc_ = CommandDesc::get(*record_->commands, command_);
			// TODO: reset hook/update desc?
			//   probably have to
		}
	} else if(mode_ == UpdateMode::commandGroup) {
		dlg_assert(record_->group);

		imGuiText("Updating from Command Group");

		auto lastRecord = record_->group->lastRecord.get();
		if(lastRecord != record_.get()) {
			record_ = record_->group->lastRecord;
			auto hierarchy = CommandDesc::findHierarchy(record_->commands, desc_);
			command_ = {hierarchy.begin(), hierarchy.end()};
			desc_ = CommandDesc::get(*record_->commands, command_);
			// TODO: reset hook/update desc?
			//   probably have to
		}
	} else if(mode_ == UpdateMode::swapchain) {
		if(!gui_->dev().swapchain) {
			record_ = {};
			records_ = {};
			swapchainCounter_ = {};
			desc_ = {};
			command_ = {};
			hook.target = {};
			hook.desc({});
			hook.unsetHookOps();
			return;
		}

		auto& sc = *gui_->dev().swapchain;

		imGuiText("Showing per-present commands from");
		ImGui::SameLine();
		refButton(*gui_, sc);

		ImGui::SameLine();
		ImGui::Checkbox("Freeze Commands", &freezePresentBatches_);

		if(swapchainCounter_ != sc.presentCounter && !freezePresentBatches_) {
			// if we have a record selected, try to find its match in the
			// new submission list
			if(record_) {
				auto newRec = IntrusivePtr<CommandRecord> {};
				for(auto& batch : sc.frameSubmissions) {
					for(auto& rec : batch.submissions) {
						// TODO: there may be multiple records with same group.
						// not sure how to handle that case.
						if(rec->group == record_->group) {
							newRec = rec;
							break;
						}
					}
				}

				record_ = newRec;
			}

			if(record_) {
				auto hierarchy = CommandDesc::findHierarchy(record_->commands, desc_);
				command_ = {hierarchy.begin(), hierarchy.end()};
				desc_ = CommandDesc::get(*record_->commands, command_);

				dlg_assert(hook.target.group == record_->group);
				if(desc_.empty()) {
					hook.desc({});
					hook.target = {};
				} else {
					// TODO: we should do this! But without invalidating
					// records I guess. Probably counts for all desc() calls
					// where a command was found
					// hook.desc(desc_);
				}
			} else {
				desc_ = {};
				command_ = {};
				hook.target = {};
				hook.desc({});
			}

			records_ = sc.frameSubmissions;
			swapchainCounter_ = sc.presentCounter;
		}
	}

	auto val = commandFlags_.value();
	ImGui::CheckboxFlags("Bind", &val, u32(CommandType::bind));
	ImGui::SameLine();
	ImGui::CheckboxFlags("Draw", &val, u32(CommandType::draw));
	ImGui::SameLine();
	ImGui::CheckboxFlags("Dispatch", &val, u32(CommandType::dispatch));
	ImGui::SameLine();
	ImGui::CheckboxFlags("Transfer", &val, u32(CommandType::transfer));
	ImGui::SameLine();
	ImGui::CheckboxFlags("Sync", &val, u32(CommandType::sync));
	ImGui::SameLine();
	ImGui::CheckboxFlags("End", &val, u32(CommandType::end));
	ImGui::SameLine();
	ImGui::CheckboxFlags("Query", &val, u32(CommandType::query));
	ImGui::SameLine();
	ImGui::CheckboxFlags("Other", &val, u32(CommandType::other));
	commandFlags_ = CommandType(val);

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

	ImGui::BeginChild("Command list", {0, 0});

	if(mode_ == UpdateMode::swapchain) {
		auto* selected = record_ && !command_.empty() ? command_.back() : nullptr;

		for(auto b = 0u; b < records_.size(); ++b) {
			auto& batch = records_[b];
			auto id = dlg::format("vkQueueSubmit:{}", b);

			if(ImGui::TreeNode(id.c_str(), "vkQueueSubmit")) {
				for(auto r = 0u; r < batch.submissions.size(); ++r) {
					auto& rec = batch.submissions[r];

					// When the record isn't valid anymore (cb is unset), we have to
					// be careful to not actually reference any destroyed resources.
					if(!rec->cb) {
						unsetDestroyedLocked(*rec);
					}

					dlg_assert(rec->destroyed.empty());

					// extra tree node for every submission
					// TODO: only show this when there is more than one and
					// then show index as well? might mess with ids though.
					auto flags = 0u;
					// auto id = rec->group;
					auto id = dlg::format("Commands:{}", r);
					if(ImGui::TreeNodeEx(id.c_str(), flags, "Commands")) {
						auto nsel = displayCommands(rec->commands, selected, commandFlags_);
						if(!nsel.empty() && (command_.empty() || nsel.back() != command_.back())) {
							record_ = rec;
							command_ = std::move(nsel);
							desc_ = CommandDesc::get(*record_->commands, command_);

							dev.commandHook->target = {};
							if(record_->group) {
								dev.commandHook->target.group = record_->group;
								dev.commandHook->desc(desc_);
							}
						}

						ImGui::TreePop();
					}
				}

				ImGui::TreePop();
			}
		}
	} else {
		dlg_assert(record_);

		// When the record isn't valid anymore (cb is unset), we have to
		// be careful to not actually reference any destroyed resources.
		if(record_ && !record_->cb) {
			unsetDestroyedLocked(*record_);
		}

		dlg_assert(record_->destroyed.empty());

		ImGui::PushID(dlg::format("{}", record_->group).c_str());

		auto* selected = command_.empty() ? nullptr : command_.back();
		auto nsel = displayCommands(record_->commands, selected, commandFlags_);
		if(!nsel.empty() && (command_.empty() || nsel.back() != command_.back())) {
			command_ = std::move(nsel);
			desc_ = CommandDesc::get(*record_->commands, command_);

			// when nothing was selected before, register the hook
			if(command_.empty()) {
				dlg_assert(
					!dev.commandHook->target.cb &&
					!dev.commandHook->target.group &&
					!dev.commandHook->target.record);

				if(mode_ == UpdateMode::none) {
					dev.commandHook->target.record = record_.get();
				} else if(mode_ == UpdateMode::commandBuffer) {
					dlg_assert(cb_);
					dev.commandHook->target.cb = cb_;
				} else if(mode_ == UpdateMode::commandGroup) {
					dlg_assert(record_->group);
					dev.commandHook->target.group = record_->group;
				}
			}

			// in any case, update the hook
			dev.commandHook->desc(desc_);
		}

		ImGui::PopID();
	}

	ImGui::EndChild();
	ImGui::TableNextColumn();

	// command info
	ImGui::BeginChild("Command Info");
	if(!command_.empty()) {
		displayInspector(draw, *command_.back());
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
	hook.desc({});
	hook.target = {};

	command_ = {};
	record_ = std::move(record);
	desc_ = {};
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
	hook.desc({});
	hook.target = {};

	command_ = {};
	record_ = std::move(record);
	desc_ = {};
}

void CommandBufferGui::showSwapchainSubmissions() {
	mode_ = UpdateMode::swapchain;
	cb_ = {};

	command_ = {};
	record_ = {};
	desc_ = {};

	// Unset hooks
	auto& hook = *gui_->dev().commandHook;
	hook.unsetHookOps();
	hook.desc({});
	hook.target = {};
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
	hook.desc({});
	hook.target = {};

	command_ = {};
	record_ = std::move(record);
	desc_ = {};
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

void CommandBufferGui::displayImage(Draw& draw, const CopiedImage& img) {
	auto& dev = gui_->dev();

	dlg_assert(img.aspectMask);
	dlg_assert(img.image);
	dlg_assert(img.imageView);

	draw.usedHookState = dev.commandHook->state;
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

void CommandBufferGui::displayInspector(Draw& draw, const Command& bcmd) {
	if(bcmd.type() == CommandType::dispatch ||
			bcmd.type() == CommandType::draw ||
			bcmd.type() == CommandType::transfer) {
		displayActionInspector(draw, bcmd);
		return;
	}

	// TODO: slight code duplication with IO inspector. Might be able to
	// refactor it
	auto& hook = *gui_->dev().commandHook;
	hook.queryTime = true;
	if(hook.state) {
		dlg_assert(record_);
		auto lastTime = hook.state->neededTime;
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

	bcmd.displayInspector(*gui_);
}

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

void CommandBufferGui::displayDs(Draw& draw, const Command& cmd) {
	auto& gui = *this->gui_;

	auto& hook = *gui.dev().commandHook;
	if(!hook.state) {
		ImGui::Text("Waiting for a submission...");
		return;
	}

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

	dlg_assert(hook.copyDS);
	auto [setID, bindingID, elemID, _] = *hook.copyDS;

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
	auto& bindingLayout = set->layout->bindings[bindingID];
	auto dsType = bindingLayout.descriptorType;
	auto dsCat = category(dsType);

	imGuiText("{}", vk::name(dsType));

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
			auto* binding = getReflectBinding(refl, setID, bindingID);
			if(!binding) {
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
			// TODO: display all samplers?, for all elements
			refButton(gui, nonNull(bindingLayout.immutableSamplers[0]));
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

		dlg_assert(elem.imageInfo.imageView);
		auto& imgView = *elem.imageInfo.imageView;
		refButton(gui, imgView);

		dlg_assert(imgView.img);
		ImGui::SameLine();
		refButton(gui, *imgView.img);

		imGuiText("Format: {}", vk::name(imgView.ci.format));
		auto& extent = imgView.img->ci.extent;
		imGuiText("Extent: {}x{}x{}", extent.width, extent.height, extent.depth);

		if(needsImageLayout(dsType)) {
			imGuiText("Layout: {}", vk::name(elem.imageInfo.layout));
		}

		displayImage(draw, *img);
	}

	// == BufferView ==
	// TODO
}

void CommandBufferGui::displayDsList(const Command& cmd) {
	auto& hook = *gui_->dev().commandHook;

	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	if(!drawCmd && !dispatchCmd) {
		return;
	}

	// TODO: this seems to need special treatment for arrays?
	// debug with tkn/deferred gbuf pass
	auto modBindingName = [&](const SpvReflectShaderModule& refl, u32 setID, u32 bindingID) -> const char* {
		auto* binding = getReflectBinding(refl, setID, bindingID);
		if(binding && binding->name && *binding->name != '\0') {
			return binding->name;
		} else if(binding && binding->type_description &&
				binding->type_description->type_name &&
				*binding->type_description->type_name != '\0') {
			return binding->type_description->type_name;
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
					hook.copyDS = {i, b, 0, true};
				}
			}

			ImGui::TreePop();
		}
	}

}

void CommandBufferGui::displayIOList(const Command& cmd) {
	auto& hook = *gui_->dev().commandHook;
	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);

	if(ImGui::Selectable("Command", hook.queryTime)) {
		hook.unsetHookOps(true);
	}

	// Transfer commands:
	if(!drawCmd && !dispatchCmd) {
		// TODO: add support for viewing buffers here.
		// Hard to do in a meaningful way though.
		auto found = false;
		auto addSrc = [&](auto* cmd) {
			if(!cmd) {
				return;
			}

			found = true;
			if(ImGui::Selectable("Source", hook.copyTransferSrc)) {
				hook.unsetHookOps();
				hook.copyTransferSrc = true;
			}
		};

		auto addDst = [&](auto* cmd) {
			if(!cmd) {
				return;
			}

			found = true;
			if(ImGui::Selectable("Destination", hook.copyTransferDst)) {
				hook.unsetHookOps();
				hook.copyTransferDst = true;
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
		//   cleared attachments
		addDst(dynamic_cast<const ClearAttachmentCmd*>(&cmd));

		dlg_assertlm(dlg_level_warn, found, "IO inspector unimplemented for command");
		return;
	}

	// Vertex IO
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

			if(indirectCmd || indirectCountCmd) {
				hook.copyIndirectCmd = true;
			}
		}
	}

	displayDsList(cmd);

	// Attachments
	if(drawCmd) {
		ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
		if(ImGui::TreeNodeEx("Attachments")) {
			const BeginRenderPassCmd* rpCmd = nullptr;
			for(auto* cmdi : command_) {
				if(rpCmd = dynamic_cast<const BeginRenderPassCmd*>(cmdi); rpCmd) {
					break;
				}
			}

			dlg_assert(rpCmd);
			if(rpCmd && rpCmd->rp) {
				auto& desc = nonNull(nonNull(rpCmd->rp).desc);
				auto subpassID = rpCmd->subpassOfDescendant(*command_.back());
				auto& subpass = desc.subpasses[subpassID];

				auto addAttachment = [&](auto label, auto id) {
					auto flags = ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
					if(hook.copyAttachment && hook.copyAttachment->id == id) {
						flags |= ImGuiTreeNodeFlags_Selected;
					}

					ImGui::TreeNodeEx(label.c_str(), flags);
					if(ImGui::IsItemClicked()) {
						hook.unsetHookOps();
						hook.copyAttachment = {id, false};
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

			ImGui::TreePop();
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
}

void CommandBufferGui::displayVertexInput(Draw& draw, const DrawCmdBase& cmd) {
	// TODO: display binding information
	// TODO: how to display indices?
	// TODO: only show vertex range used for draw call

	auto& hook = *gui_->dev().commandHook;
	if(hook.state->vertexBufCopies.size() < cmd.state.pipe->vertexBindings.size()) {
		if(!hook.state->errorMessage.empty()) {
			imGuiText("Error: {}", hook.state->errorMessage);
		} else {
			ImGui::Text("Error: not enough vertex buffers bound");
		}

		return;
	}


	auto& pipe = *cmd.state.pipe;

	SpvReflectShaderModule* vertStage = nullptr;
	for(auto& stage : pipe.stages) {
		if(stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
			vertStage = &nonNull(nonNull(stage.spirv).reflection);
			break;
		}
	}

	if(!vertStage) {
		// TODO: yeah this can happen with mesh shaders now
		ImGui::Text("Grahpics Pipeline has no vertex stage :o");
		return;
	}

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

	// TODO sort attribs by input location?

	auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable;
	if(ImGui::BeginChild("vertexTable", {0.f, 200.f})) {
		if(attribs.empty()) {
			ImGui::Text("No Vertex input");
		} else if(ImGui::BeginTable("Vertices", int(attribs.size()), flags)) {
			for(auto& attrib : attribs) {
				auto& iv = *vertStage->input_variables[attrib.second];
				ImGui::TableSetupColumn(iv.name);
			}

			ImGui::TableHeadersRow();
			ImGui::TableNextRow();

			auto finished = false;
			auto id = 0u;
			while(!finished && id < 100) {
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

					if(off + size > buf.copy.size()) {
						finished = true;
						break;
					}

					auto* ptr = buf.copy.data() + off;
					auto str = readFormat(attrib.format, {ptr, size});

					imGuiText("{}", str);
				}

				++id;
				ImGui::TableNextRow();
			}

			ImGui::EndTable();
		}
	}

	ImGui::EndChild();

	// 2: viewer
	if(ImGui::BeginChild("vertexViewer")) {
		auto avail = ImGui::GetContentRegionAvail();
		auto pos = ImGui::GetCursorScreenPos();

		vertexDrawData_.self = this;
		vertexDrawData_.cb = draw.cb;
		vertexDrawData_.offset = {pos.x, pos.y};
		vertexDrawData_.size = {avail.x, avail.y};
		vertexDrawData_.cmd = &cmd;

		auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
			auto* data = static_cast<VertexDrawData*>(cmd->UserCallbackData);
			auto& hook = *data->self->gui_->dev().commandHook;
			auto& pipe = *data->cmd->state.pipe;

			std::optional<VkIndexType> indexType;
			u32 offset {};
			u32 drawCount {};
			u32 vertexOffset {};

			if(auto* dcmd = dynamic_cast<const DrawCmd*>(data->cmd); dcmd) {
				offset = dcmd->firstVertex;
				drawCount = dcmd->vertexCount;
			} else if(auto* dcmd = dynamic_cast<const DrawIndexedCmd*>(data->cmd); dcmd) {
				offset = dcmd->firstIndex;
				vertexOffset = dcmd->vertexOffset;
				drawCount = dcmd->indexCount;
				indexType = dcmd->state.indices.type;
			} else if(auto* dcmd = dynamic_cast<const DrawIndirectCmd*>(data->cmd); dcmd) {
				dlg_assert(hook.copyIndirectCmd);
				if(!hook.state) {
					ImGui::Text("Indirect command not loaded yet...");
					return;
				}

				auto& ic = hook.state->indirectCopy;
				auto span = ReadBuf(ic.copy);
				if(dcmd->indexed) {
					auto ecmd = read<VkDrawIndexedIndirectCommand>(span);
					offset = ecmd.firstIndex;
					drawCount = ecmd.indexCount;
					vertexOffset = ecmd.vertexOffset;
					indexType = dcmd->state.indices.type;
				} else {
					auto ecmd = read<VkDrawIndirectCommand>(span);
					offset = ecmd.firstVertex;
					drawCount = ecmd.vertexCount;
				}

			} else {
				// TODO: DrawIndirectCount
				dlg_info("Vertex viewer unimplemented for command type");
				return;
			}

			data->self->vertexViewer_.imGuiDraw(data->cb, pipe,
				*hook.state, indexType, offset, drawCount, vertexOffset,
				data->offset, data->size);
		};

		ImGui::GetWindowDrawList()->AddCallback(cb, &vertexDrawData_);
		ImGui::InvisibleButton("Canvas", avail);

		vertexViewer_.updateInput(gui_->dt());
	}

	ImGui::EndChild();
}

void CommandBufferGui::displayVertexOutput(Draw& draw, const DrawCmdBase& cmd) {
	auto& hook = *gui_->dev().commandHook;
	if(!cmd.state.pipe->xfbPatched) {
		imGuiText("Error: couldn't inject transform feedback code to shader");
		return;
	} else if(!hook.state->transformFeedback.buffer.size) {
		if(!hook.state->errorMessage.empty()) {
			imGuiText("Transform feedback error: {}", hook.state->errorMessage);
		} else {
			ImGui::Text("Error: no transform feedback");
		}

		return;
	}

	u32 vertexCount {};
	if(auto* dcmd = dynamic_cast<const DrawCmd*>(&cmd); dcmd) {
		vertexCount = dcmd->vertexCount * dcmd->instanceCount;
	} else if(auto* dcmd = dynamic_cast<const DrawIndexedCmd*>(&cmd); dcmd) {
		vertexCount = dcmd->indexCount * dcmd->instanceCount;
	} else if(auto* dcmd = dynamic_cast<const DrawIndirectCmd*>(&cmd); dcmd) {
		dlg_assert(hook.copyIndirectCmd);
		if(!hook.state) {
			ImGui::Text("Indirect command not loaded yet...");
			return;
		}

		auto& ic = hook.state->indirectCopy;
		auto span = ReadBuf(ic.copy);
		if(dcmd->indexed) {
			auto ecmd = read<VkDrawIndexedIndirectCommand>(span);
			vertexCount = ecmd.indexCount * ecmd.instanceCount;
		} else {
			auto ecmd = read<VkDrawIndirectCommand>(span);
			vertexCount = ecmd.vertexCount * ecmd.instanceCount;
		}

	} else {
		// TODO: DrawIndirectCount
		imGuiText("Vertex viewer unimplemented for command type");
		return;
	}

	auto xfbData = ReadBuf(hook.state->transformFeedback.copy);
	vertexCount = std::min(vertexCount, u32(xfbData.size() / 16u));

	// 1: table
	auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable;
	if(ImGui::BeginChild("vertexTable", {0.f, 200.f})) {
		if(ImGui::BeginTable("Vertices", 1, flags)) {
			ImGui::TableSetupColumn("Builtin Position");
			ImGui::TableHeadersRow();
			ImGui::TableNextRow();

			for(auto i = 0u; i < std::min(u32(100u), vertexCount); ++i) {
				ImGui::TableNextColumn();

				auto pos = read<Vec4f>(xfbData);
				imGuiText("{}", pos);

				ImGui::TableNextRow();
			}

			ImGui::EndTable();
		}
	}

	ImGui::EndChild();

	// 2: viewer
	if(ImGui::BeginChild("vertexViewer")) {
		auto avail = ImGui::GetContentRegionAvail();
		auto pos = ImGui::GetCursorScreenPos();

		vertexDrawData_.self = this;
		vertexDrawData_.cb = draw.cb;
		vertexDrawData_.offset = {pos.x, pos.y};
		vertexDrawData_.size = {avail.x, avail.y};
		vertexDrawData_.cmd = &cmd;
		vertexDrawData_.vertexCount = vertexCount;

		auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
			auto* data = static_cast<VertexDrawData*>(cmd->UserCallbackData);
			auto& hook = *data->self->gui_->dev().commandHook;

			auto& xfbBuf = hook.state->transformFeedback.buffer;

			VkVertexInputBindingDescription vertexBinding {
				0u, sizeof(Vec4f), VK_VERTEX_INPUT_RATE_VERTEX,
			};

			VkVertexInputAttributeDescription vertexAttrib {
				0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u,
			};

			VertexViewer::DrawData drawData {};
			drawData.drawCount = data->vertexCount;
			drawData.vertexBuffers = {{{xfbBuf.buf, 0u, xfbBuf.size}}};
			drawData.vertexInfo.vertexAttributeDescriptionCount = 1u;
			drawData.vertexInfo.pVertexAttributeDescriptions = &vertexAttrib;
			drawData.vertexInfo.vertexBindingDescriptionCount = 1u;
			drawData.vertexInfo.pVertexBindingDescriptions = &vertexBinding;
			drawData.canvasOffset = data->offset;
			drawData.canvasSize = data->size;
			drawData.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			data->self->vertexViewer_.imGuiDraw(data->cb, drawData);
		};

		ImGui::GetWindowDrawList()->AddCallback(cb, &vertexDrawData_);
		ImGui::InvisibleButton("Canvas", avail);

		vertexViewer_.updateInput(gui_->dt());

		// we read from the buffer that is potentially written again
		// by the hook so we need barriers.
		draw.usedHookState = gui_->dev().commandHook->state;
		dlg_assert(draw.usedHookState);
	}

	ImGui::EndChild();
}

void CommandBufferGui::displayVertexViewer(Draw& draw, const Command& cmd) {
	auto& hook = *gui_->dev().commandHook;
	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	dlg_assert(drawCmd);

	if(!drawCmd || !drawCmd->state.pipe) {
		ImGui::Text("Pipeline was destroyed, can't interpret state");
		return;
	} else if(!hook.state) {
		ImGui::Text("Waiting for a submission...");
		return;
	}

	// 1: table
	if(ImGui::BeginTabBar("Stage")) {
		if(ImGui::BeginTabItem("Vertex input")) {
			displayVertexInput(draw, *drawCmd);
			ImGui::EndTabItem();
		}

		if(ImGui::BeginTabItem("Vertex Output")) {
			displayVertexOutput(draw, *drawCmd);
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void CommandBufferGui::displaySelectedIO(Draw& draw, const Command& cmd) {
	auto& hook = *gui_->dev().commandHook;

	auto* drawCmd = dynamic_cast<const DrawCmdBase*>(&cmd);
	auto* dispatchCmd = dynamic_cast<const DispatchCmdBase*>(&cmd);

	// TODO: display more information, not just raw data
	//   e.g. link to the respective resources, descriptor sets etc
	//   maybe allow to select the descriptorSet entry to get to the ds?
	auto cmdInfo = true;
	if(hook.copyDS) {
		// TODO: only show this for bound resources that may be written,
		// i.e. storage buffers/images
		if(ImGui::Checkbox("Before Command", &hook.copyDS->before)) {
			hook.invalidateRecordings();
			hook.invalidateData();
		}

		displayDs(draw, cmd);
		cmdInfo = false;
	} else if(hook.copyVertexBuffers) {
		displayVertexViewer(draw, cmd);
		cmdInfo = false;
	} else if(hook.copyAttachment) {
		dlg_assert(drawCmd);

		// TODO: only show this button for output attachments (color, depthStencil)
		// does not make sense otherwise as it stays the same i guess
		if(ImGui::Checkbox("Before Command", &hook.copyAttachment->before)) {
			hook.invalidateRecordings();
			hook.invalidateData();
		}

		if(!drawCmd->state.fb) {
			imGuiText("Framebuffer was destroyed");
			return;
		}

		// information
		auto aid = hook.copyAttachment->id;
		auto& fb = nonNull(drawCmd->state.fb);
		dlg_assert(aid < fb.attachments.size());
		auto& view = nonNull(fb.attachments[aid]);
		auto& img = nonNull(view.img);

		refButton(*gui_, fb);
		refButton(*gui_, view);
		refButton(*gui_, img);

		if(hook.state) {
			if(hook.state->attachmentCopy.image) {
				displayImage(draw, hook.state->attachmentCopy);
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
	} else if(hook.copyTransferSrc || hook.copyTransferDst) {
		if(hook.state) {
			// TODO: only show where it makes sense?
			// shouldn't be here for src resources i guess
			if(ImGui::Checkbox("Before Command", &hook.copyTransferBefore)) {
				hook.invalidateRecordings();
				hook.invalidateData();
			}

			auto refSrcDst = [&](auto* ccmd) {
				if(!ccmd) {
					return;
				}

				if(hook.copyTransferSrc) {
					refButtonExpect(*gui_, ccmd->src);
				} else  {
					refButtonExpect(*gui_, ccmd->dst);
				}
			};

			auto refDst = [&](auto* ccmd) {
				if(!ccmd) {
					return;
				}

				dlg_assert(!hook.copyTransferSrc);
				refButtonExpect(*gui_, ccmd->dst);
			};

			auto refSrc = [&](auto* ccmd) {
				if(!ccmd) {
					return;
				}

				dlg_assert(hook.copyTransferSrc);
				refButtonExpect(*gui_, ccmd->src);
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
			if(auto* ccmd = dynamic_cast<const ClearAttachmentCmd*>(&cmd); ccmd) {
				// TODO
				(void) ccmd;
			}

			if(hook.state->transferImgCopy.image) {
				displayImage(draw, hook.state->transferImgCopy);
			} else {
				imGuiText("Error: {}", hook.state->errorMessage);
			}
		} else {
			ImGui::Text("Waiting for a submission...");
		}

		cmdInfo = false;
	}

	if(cmdInfo) {
		hook.queryTime = true;

		if(dynamic_cast<const DispatchIndirectCmd*>(&cmd) ||
				dynamic_cast<const DrawIndirectCmd*>(&cmd) ||
				dynamic_cast<const DrawIndirectCountCmd*>(&cmd)) {
			hook.copyIndirectCmd = true;
		}

		if(hook.state) {
			dlg_assert(record_);
			auto lastTime = hook.state->neededTime;
			auto validBits = gui_->dev().queueFamilies[record_->queueFamily].props.timestampValidBits;
			if(validBits == 0u) {
				dlg_assert(lastTime == u64(-1));
				imGuiText("Time: unavailable (Queue family does not support timing queries)");
			} else {
				auto displayDiff = lastTime * gui_->dev().props.limits.timestampPeriod;
				displayDiff /= 1000.f * 1000.f;
				imGuiText("Time: {} ms", displayDiff);
			}

			// for indirect commands, show effective command
			if(hook.copyIndirectCmd && hook.state->indirectCopy.buffer.size) {
				auto& ic = hook.state->indirectCopy;
				auto span = ReadBuf(ic.copy);
				if(auto* dcmd = dynamic_cast<const DrawIndirectCmd*>(&cmd)) {
					if(dcmd->indexed) {
						auto ecmd = read<VkDrawIndexedIndirectCommand>(span);
						imGuiText("firstIndex: {}", ecmd.firstIndex);
						imGuiText("indexCount: {}", ecmd.indexCount);
						imGuiText("vertexOffset: {}", ecmd.vertexOffset);
						imGuiText("firstInstance: {}", ecmd.firstInstance);
						imGuiText("instanceCount: {}", ecmd.instanceCount);
					} else {
						auto ecmd = read<VkDrawIndirectCommand>(span);
						imGuiText("firstVertex: {}", ecmd.firstVertex);
						imGuiText("vertexCount: {}", ecmd.vertexCount);
						imGuiText("firstInstance: {}", ecmd.firstInstance);
						imGuiText("instanceCount: {}", ecmd.instanceCount);
					}
				} else if(dynamic_cast<const DispatchIndirectCmd*>(&cmd)) {
					auto ecmd = read<VkDispatchIndirectCommand>(span);
					imGuiText("groups X: {}", ecmd.x);
					imGuiText("groups Y: {}", ecmd.y);
					imGuiText("groups Z: {}", ecmd.z);
				} // TODO: drawIndirectCount
			}
		}

		cmd.displayInspector(*gui_);
	}
}

// If it returns true, cmd should display own command stuff in second window
void CommandBufferGui::displayActionInspector(Draw& draw, const Command& cmd) {
	// TODO: don't even display the inspector when we are viewing a static
	// record and that record is invalidated.
	// (or other cases where we know it will never be submitted again)
	// auto& hook = *gui.dev().commandHook;
	// if(hook ... invalid?)
	// 	return true;
	// }

	auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
	if(!ImGui::BeginTable("IO inspector", 2, flags, ImGui::GetContentRegionAvail())) {
		return;
	}

	ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, 250.f);
	ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	ImGui::BeginChild("Command IO list");

	displayIOList(cmd);

	ImGui::EndChild();
	ImGui::TableNextColumn();
	ImGui::BeginChild("Command IO Inspector");

	displaySelectedIO(draw, cmd);

	ImGui::EndChild();
	ImGui::EndTable();
}

} // namespace vil
