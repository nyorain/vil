#pragma once

#include <nytl/span.hpp>
#include <util/linalloc.hpp>
#include <string>
#include <vector>

namespace backward {

struct SourceLoc {
	std::string function;
	std::string filename;
	unsigned line {};
	unsigned col {};

	bool operator==(const SourceLoc &b) const {
		return function == b.function &&
			filename == b.filename &&
			line == b.line &&
			col == b.col;
	}

	bool operator!=(const SourceLoc &b) const {
		return !(*this == b);
	}
};

// TODO: interface could be more efficient, avoiding allocations.
// But not needed atm, only in gui code.
std::vector<SourceLoc> resolve(vil::LinAllocScope&, vil::span<void*> address);

} // namespace backward
