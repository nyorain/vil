#pragma once

#include <fwd.hpp>
#include <imgui/textedit.h>
#include <nytl/bytes.hpp>

namespace vil {

struct BufferViewer {
	Gui* gui;
	igt::TextEditor textedit;

	void init(Gui& gui);
	void display(ReadBuf data);
};

} // namespace vil


