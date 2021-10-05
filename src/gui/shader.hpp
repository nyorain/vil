#pragma once

#include <fwd.hpp>
#include <imgui/textedit.h>
#include <spvm/program.h>
#include <spvm/state.h>
#include <spvm/result.h>

namespace vil {

struct ShaderDebugger {
	Gui* gui;
	igt::TextEditor textedit;

	spvm_program_t program;
	spvm_state_t state;

	void display();
};

} // namespace vil

