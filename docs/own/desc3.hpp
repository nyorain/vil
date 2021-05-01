#pragma once

#include <fwd.hpp>
#include <commands.hpp>
#include <vector>

namespace vil {

std::vector<Command*> find(Command* root, span<const Command*> dst) {
	auto it = root;
	while(it) {
		it = it->next;
	}
}

} // namespace vil
