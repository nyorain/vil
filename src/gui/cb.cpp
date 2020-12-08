#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <imguiutil.hpp>
#include <commands.hpp>
#include <imgui/imgui.h>

namespace fuen {

void CommandBufferGui::draw() {
	// Command list
	ImGui::BeginChild("Command list", {400, 0});

	// We can only display the content when the command buffer is in
	// executable state. The state of the command buffer is protected
	// by the device mutex (also command_ and the cb itself).
	if(!cb_) {
		ImGui::Text("No command buffer selected");
		return;
	}

	if(cb_->state == CommandBuffer::State::executable) {
		ImGui::PushID(dlg::format("{}:{}", cb_, cb_->resetCount).c_str());

		// TODO: add selector ui to filter out various commands/don't
		// show sections etc. Should probably pass a struct DisplayDesc
		// to displayCommands instead of various parameters
		auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
		auto* nsel = displayCommands(cb_->commands, command_, flags);
		if(nsel) {
			resetCount_ = cb_->resetCount;
			command_ = nsel;
		}

		if(cb_->resetCount != resetCount_) {
			command_ = nullptr;
		}

		ImGui::PopID();
	} else {
		ImGui::Text("[Not in exeuctable state]");
	}

	ImGui::EndChild();
	ImGui::SameLine();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	if(command_) {
		command_->displayInspector(*gui_);
	}

	ImGui::EndChild();
}

void CommandBufferGui::select(CommandBuffer& cb) {
	cb_ = &cb;
	command_ = {};
	resetCount_ = cb_->resetCount;
}

void CommandBufferGui::destroyed(const Handle& handle) {
	if(&handle == cb_) {
		cb_ = nullptr;
		command_ = nullptr;
		resetCount_ = {};
	}
}

} // namespace fuen
