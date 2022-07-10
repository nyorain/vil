#pragma once

#include <memory>
#include <imgui/imgui.h>
#include <util/linalloc.hpp>

namespace vil {

struct FLCS;

// viz
struct VizLCS {
	VizLCS();
	~VizLCS();

	void draw();

	std::unique_ptr<FLCS> algo_;
};

} // namespace vil
