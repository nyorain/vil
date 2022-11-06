#pragma once

#include <fwd.hpp>

namespace vil {

class EventViewer {
public:
	void draw(Gui& gui);

protected:
	double offset_ {};
	double scale_ {10.f}; // seconds
};

} // namespace
