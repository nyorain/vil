#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <gui/commandHook.hpp>
#include <gui/util.hpp>
#include <queue.hpp>
#include <cb.hpp>
#include <commands.hpp>
#include <record.hpp>
#include <util/bytes.hpp>
#include <util/util.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>

namespace fuen {

// CommandBufferGui
CommandBufferGui::CommandBufferGui() {
	commandFlags_ = CommandType(~(CommandType::end | CommandType::bind | CommandType::query));
	ioImage_.flags = DrawGuiImage::flagMaskR |
		DrawGuiImage::flagMaskG |
		DrawGuiImage::flagMaskB;
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
	ImGui::BeginChild("Command Info");
	if(!command_.empty()) {
		command_.back()->displayInspector(*gui_);
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

	// TODO: when a new CopiedImage is displayed we could reset the
	//   color mask flags. In some cases this is desired but probably
	//   not in all.

	fuen::displayImage(*gui_, ioImage_, img.extent, minImageType(img.extent),
		img.format, img.srcSubresRange);

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

} // namespace fuen
