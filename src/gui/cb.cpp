#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <gui/util.hpp>
#include <queue.hpp>
#include <cb.hpp>
#include <commands.hpp>
#include <record.hpp>
#include <util/bytes.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>

namespace fuen {

// CommandBufferGui
CommandBufferGui::CommandBufferGui() {
	commandFlags_ = CommandType(~(CommandType::end | CommandType::bind));
}

CommandBufferGui::~CommandBufferGui() = default;

void CommandBufferGui::draw(Draw& draw) {
	if(!record_) {
		ImGui::Text("No record selected");
		return;
	}

	draw_ = &draw;
	// auto& dev = gui_->dev();

	if(!record_->cb) {
		unsetDestroyedLocked(*record_);
	} else {
		dlg_assert(record_->destroyed.empty());
	}

	if(record_->group) {
		// TODO: (un)install hook when toggled
		ImGui::Checkbox("Update", &updateFromGroup_);
	} else {
		// disabled button
		ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);

		ImGui::Checkbox("Update", &updateFromGroup_);
		if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::BeginTooltip();
			ImGui::Text("Recording does not have a group");
			ImGui::EndTooltip();
		}

		ImGui::PopStyleVar();
		ImGui::PopItemFlag();
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
	ImGui::Columns(2);
	if(!columnWidth0_) {
		ImGui::SetColumnWidth(-1, 250.f);
		columnWidth0_ = true;
	}

	ImGui::BeginChild("Command list", {0, 0});

	if(updateFromGroup_ && record_->group) {
		auto lastRecord = record_->group->lastRecord.get();
		if(lastRecord != record_.get()) {
			record_ = record_->group->lastRecord;
			auto hierarchy = CommandDesc::findHierarchy(record_->commands, desc_);
			command_ = {hierarchy.begin(), hierarchy.end()};
			// TODO: reset hook/update desc?
			//   probably have to
		}
	} else if(!updateFromGroup_ && record_->group && (record_->group->hook || hook_)) {
		dlg_assert(record_->group->hook.get() == hook_);
		record_->group->hook.reset();
		hook_ = nullptr;
	}

	ImGui::PushID(dlg::format("{}", record_->group).c_str());

	auto* selected = command_.empty() ? nullptr : command_.back();
	auto nsel = displayCommands(record_->commands, selected, commandFlags_);
	if(!nsel.empty() && (command_.empty() || nsel.back() != command_.back())) {
		command_ = std::move(nsel);
		desc_ = CommandDesc::get(*record_->commands, command_);

		if(!hook_) {
			if(record_->group) {
				hook_ = new CommandHook();
				record_->group->hook.reset(hook_);
			}
		}

		if(hook_) {
			hook_->desc(desc_);
		}
	}

	ImGui::PopID();

	ImGui::EndChild();
	ImGui::NextColumn();

	// command info
	ImGui::BeginChild("Command Info", {0, 0});
	if(!command_.empty()) {
		// Inspector
		command_.back()->displayInspector(*gui_);

		/*
		// Show own general gui
		if(hook_) {
			// TODO: separate count and valid flag
			if(hook_->indirect.count) {
				auto span = ReadBuf(hook_->indirect.data);
				if(auto* dcmd = dynamic_cast<const DrawIndirectCmd*>(command_)) {
					if(dcmd->indexed) {
						auto cmd = read<VkDrawIndexedIndirectCommand>(span);
						imGuiText("firstIndex: {}", cmd.firstIndex);
						imGuiText("indexCount: {}", cmd.indexCount);
						imGuiText("vertexOffset: {}", cmd.vertexOffset);
						imGuiText("firstInstance: {}", cmd.firstInstance);
						imGuiText("instanceCount: {}", cmd.instanceCount);
					} else {
						auto cmd = read<VkDrawIndirectCommand>(span);
						imGuiText("firstVertex: {}", cmd.firstVertex);
						imGuiText("vertexCount: {}", cmd.vertexCount);
						imGuiText("firstInstance: {}", cmd.firstInstance);
						imGuiText("instanceCount: {}", cmd.instanceCount);
					}
				} else if(dynamic_cast<const DispatchIndirectCmd*>(command_)) {
					auto cmd = read<VkDispatchIndirectCommand>(span);
					imGuiText("groups X: {}", cmd.x);
					imGuiText("groups Y: {}", cmd.y);
					imGuiText("groups Z: {}", cmd.z);
				} // TODO: drawIndirectCount
			}

			auto lastTime = hook_->lastTime;
			auto displayDiff = lastTime * dev.props.limits.timestampPeriod;
			auto timeNames = {"ns", "mus", "ms", "s"};

			auto it = timeNames.begin();
			while(displayDiff > 1000.f && (it + 1) != timeNames.end()) {
				++it;
				displayDiff /= 1000.f;

			}

			imGuiText("Time: {} {}", displayDiff, *it);

			if(hook_->image) {
				// TODO: unset at some point...
				// TODO: we can't be certain the previous cb using the
				//   old imageCopy_ has been finished
				// we should unset this when its no longer needed and the
				// last command needing it has finished.
				// Should probably add a mechanism for associating
				// this resource with the draw.
				imageCopy_ = hook_->image;
				draw.keepAliveImageCopy = imageCopy_;

				VkDescriptorImageInfo dsii {};
				dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				dsii.imageView = hook_->image->imageView;
				dsii.sampler = dev.renderData->nearestSampler;

				VkWriteDescriptorSet write {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.descriptorCount = 1u;
				write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				write.dstSet = draw.dsSelected;
				write.pImageInfo = &dsii;

				dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);

				// TODO
				ImGui::Spacing();
				ImGui::Spacing();

				ImVec2 pos = ImGui::GetCursorScreenPos();

				float aspect = float(hook_->image->width) / hook_->image->height;

				// TODO: this logic might lead to problems for 1xHUGE images
				float regW = ImGui::GetContentRegionAvail().x - 20.f;
				float regH = regW / aspect;

				// TODO
				imgDraw_.type = DrawGuiImage::Type::e2d;
				imgDraw_.flags =
					DrawGuiImage::flagMaskR |
					DrawGuiImage::flagMaskG |
					DrawGuiImage::flagMaskB; // |
					// DrawGuiImage::flagMaskA;
				ImGui::Image((void*) &imgDraw_, {regW, regH});

				// Taken pretty much just from the imgui demo
				auto& io = gui_->imguiIO();
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					float region_sz = 64.0f;
					float region_x = io.MousePos.x - pos.x - region_sz * 0.5f;
					float region_y = io.MousePos.y - pos.y - region_sz * 0.5f;
					float zoom = 4.0f;
					if (region_x < 0.0f) { region_x = 0.0f; }
					else if (region_x > regW - region_sz) { region_x = regW - region_sz; }
					if (region_y < 0.0f) { region_y = 0.0f; }
					else if (region_y > regH - region_sz) { region_y = regH - region_sz; }
					ImGui::Text("Min: (%.2f, %.2f)", region_x, region_y);
					ImGui::Text("Max: (%.2f, %.2f)", region_x + region_sz, region_y + region_sz);
					ImVec2 uv0 = ImVec2((region_x) / regW, (region_y) / regH);
					ImVec2 uv1 = ImVec2((region_x + region_sz) / regW, (region_y + region_sz) / regH);
					ImGui::Image((void*) &imgDraw_, ImVec2(region_sz * zoom, region_sz * zoom), uv0, uv1);
					ImGui::EndTooltip();
				}
			}
		}
		*/
	}

	ImGui::EndChild();
	ImGui::Columns();
}

void CommandBufferGui::select(IntrusivePtr<CommandRecord> record,
		bool updateFromGroup) {
	updateFromGroup_ = updateFromGroup;

	// Reset old time hooks
	if(record_) {
		if(record_->group) {
			record_->group->hook = {};
			hook_ = nullptr;
		}
	} else {
		dlg_assert(!hook_);
	}

	command_ = {};

	// NOTE: we could try to find new command matching old description
	// if(record_ && !desc_.empty()) {
	// 	command_ = CommandDesc::find(record_->commands, desc_);
	// }

	record_ = std::move(record);
	desc_ = {};
}

void CommandBufferGui::destroyed(const Handle& handle) {
	(void) handle;
	// we don't care as we only deal with recordings that have shared
	// ownership, i.e. are kept alive by us.
}

void CommandBufferGui::displayImage(const CopiedImage& img) {
	auto& dev = gui_->dev();
	auto& draw = *draw_;

	draw.usedHookState = hook_->state;

	VkDescriptorImageInfo dsii {};
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = img.imageView;
	dsii.sampler = dev.renderData->nearestSampler;

	VkWriteDescriptorSet write {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = draw.dsSelected;
	write.pImageInfo = &dsii;

	dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);

	// TODO
	ImGui::Spacing();
	ImGui::Spacing();

	ImVec2 pos = ImGui::GetCursorScreenPos();

	float aspect = float(img.extent.width) / img.extent.height;

	// TODO: this logic might lead to problems for 1xHUGE images
	float regW = ImGui::GetContentRegionAvail().x - 20.f;
	float regH = regW / aspect;

	// TODO
	imgDraw_.type = DrawGuiImage::Type::e2d;
	imgDraw_.flags =
		DrawGuiImage::flagMaskR |
		DrawGuiImage::flagMaskG |
		DrawGuiImage::flagMaskB; // |
		// DrawGuiImage::flagMaskA;
	ImGui::Image((void*) &imgDraw_, {regW, regH});

	// Taken pretty much just from the imgui demo
	auto& io = gui_->imguiIO();
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		float region_sz = 64.0f;
		float region_x = io.MousePos.x - pos.x - region_sz * 0.5f;
		float region_y = io.MousePos.y - pos.y - region_sz * 0.5f;
		float zoom = 4.0f;
		if (region_x < 0.0f) { region_x = 0.0f; }
		else if (region_x > regW - region_sz) { region_x = regW - region_sz; }
		if (region_y < 0.0f) { region_y = 0.0f; }
		else if (region_y > regH - region_sz) { region_y = regH - region_sz; }
		ImGui::Text("Min: (%.2f, %.2f)", region_x, region_y);
		ImGui::Text("Max: (%.2f, %.2f)", region_x + region_sz, region_y + region_sz);
		ImVec2 uv0 = ImVec2((region_x) / regW, (region_y) / regH);
		ImVec2 uv1 = ImVec2((region_x + region_sz) / regW, (region_y + region_sz) / regH);
		ImGui::Image((void*) &imgDraw_, ImVec2(region_sz * zoom, region_sz * zoom), uv0, uv1);
		ImGui::EndTooltip();
	}
}

} // namespace fuen
