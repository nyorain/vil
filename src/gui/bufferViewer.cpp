#include <gui/bufferViewer.hpp>
#include <gui/gui.hpp>
#include <util/buffmt.hpp>
#include <threadContext.hpp>

namespace vil {

void BufferViewer::init(Gui& gui) {
	this->gui = &gui;

	// TODO: use own lang definition instead of just GLSL. We support
	// hlsl keywords, for instance.
	const auto& lang = igt::TextEditor::LanguageDefinition::GLSLFast();
	textedit.SetLanguageDefinition(lang);

	textedit.SetShowWhitespaces(false);
	textedit.SetTabSize(4);
}

void BufferViewer::display(ReadBuf data) {
	auto layoutText = textedit.GetText();

	// NOTE: textedit seems to always append '\n' leading to issues
	// with the error marker (they may be reported in the non-existent
	// last line).
	if(layoutText.back() == '\n') {
		layoutText.pop_back();
	}

	ThreadMemScope tms;
	auto parseRes = parseType(layoutText, tms.customUse());

	igt::TextEditor::ErrorMarkers markers;
	if(parseRes.error) {
		auto& err = *parseRes.error;

		auto msg = err.message;
		msg += "\n";

		// TODO: make it work with tabs
		auto& line = err.loc.lineContent;
		auto tabCount = std::count(line.begin(), line.end(), '\t');
		msg += line;
		msg += "\n";

		// hard to say what tab size is... eh. Maybe just replace it?
		auto col = err.loc.col + tabCount * (4 - 1);
		for(auto i = 1u; i < col; ++i) {
			msg += " ";
		}

		msg += "^\n";

		// dlg_error("{}:{}: {}", err.loc.line, err.loc.col, msg);
		// dlg_error("input: '{}'", layoutText);
		markers.insert({err.loc.line, msg});
	}

	textedit.SetErrorMarkers(markers);

	ImGui::PushFont(gui->monoFont);
	textedit.Render("Layout", {0, 200});
	ImGui::PopFont();

	auto type = parseRes.type;
	if(type && !type->members.empty()) {
		displayTable("Content", *type, data);
	}
}

} // namespace vil
