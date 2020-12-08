#pragma once

#include <fwd.hpp>
#include <imgui/imgui.h>
#include <dlg/dlg.hpp>
#include <string>

namespace fuen {

struct Row {
	const char* name;
	std::string content;

	template<typename... Args>
	Row(const char* rname, const char* fmt, Args&&... args) :
			name(rname), content(dlg::format(fmt, std::forward<Args>(args)...)) {
	}
};

template<typename... Args>
inline void imGuiText(const char* fmt, Args&&... args) {
	ImGui::Text("%s", dlg::format(fmt, std::forward<Args>(args)...).c_str());
}

inline void asColumns2(span<const Row> rows) {
	ImGui::Columns(2);
	for(auto& row : rows) {
		ImGui::Text("%s", row.name);
		ImGui::NextColumn();
		ImGui::Text("%s", row.content.c_str());
		ImGui::NextColumn();
	}

	ImGui::Columns();
}

inline bool imGuiTextInput(const char* label, std::string& output) {
	// Yes, there is a lot of ugliness here.
	auto callback = [](ImGuiInputTextCallbackData* data) {
		if(data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
			auto& output = *static_cast<std::string*>(data->UserData);
			dlg_assert(output.c_str() == data->Buf);
			output.resize(data->BufTextLen);
			data->Buf = (char*) output.c_str();
		}

		return 0;
	};

	auto ret = ImGui::InputText(label, (char*) output.c_str(), output.capacity() + 1,
		ImGuiInputTextFlags_CallbackResize, callback, (void*) &output);
	return ret;
}

} // namesapce fuen
