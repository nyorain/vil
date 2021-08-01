#pragma once

#include <imgui/textedit.h>
#include <util/bytes.hpp>

namespace vil {

struct BufferViewer {
	Gui* gui;
	igt::TextEditor textedit;

	void init(Gui& gui);
	void display(ReadBuf data);
};

} // namespace vil


