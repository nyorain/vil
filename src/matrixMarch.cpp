#include <matrixMarch.hpp>

namespace vil {

MatchMatrixMarch::MatchMatrixMarch(u32 width, u32 height, LinAllocator& alloc, Matcher matcher) :
	width_(width), height_(height), alloc_(alloc) {
}

bool step();

} // namespace vil
