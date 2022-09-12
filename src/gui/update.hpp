#pragma once

#include <chrono>

namespace vil {

struct UpdateTicker {
	using Clock = std::chrono::steady_clock;

	// Returns whether an update should be done
	bool tick() {
		auto now = Clock::now();
		if(now - last_ < interval) {
			return false;
		}

		last_ = now;
		return true;
	}

	// Makes sure the ticker triggers in the next tick
	void activate() {
		last_ = Clock::now() - interval;
	}

	Clock::duration interval {}; // 0 interval, always returns true
	Clock::time_point last_ {};
};

} // namespace vil
