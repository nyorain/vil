#pragma once

#include <memory>
#include <lmm.hpp>
#include <imgui/imgui.h>
#include <util/linalloc.hpp>

namespace vil {

// viz
struct VizLCS {
	VizLCS();
	void draw();

	LinAllocator alloc_;
	LazyMatrixMarch algo_;
	std::unique_ptr<float[]> weights_;
};

} // namespace vil
