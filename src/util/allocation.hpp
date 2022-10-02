#pragma once

#include <fwd.hpp>
#include <cstddef> // std::size_t
#include <cmath> // std::ceil
#include <algorithm> // std::clamp
#include <type_traits> // std::is_unsigned
#include <util/dlg.hpp>

namespace vil {

/// Utility struct that represents an allocated range (offset + size).
template<typename Size>
struct BasicAllocation {
	Size offset {0};
	Size size {0};
};

/// Returns the end of the given allocation (i.e. one-past-end address)
template<typename Size>
constexpr Size end(const BasicAllocation<Size>& a) {
	return a.offset + a.size;
}

template<typename S> constexpr bool
operator==(const BasicAllocation<S>& a, const BasicAllocation<S>& b) {
	return a.offset == b.offset && a.size == b.size;
}

template<typename S> constexpr bool
operator!=(const BasicAllocation<S>& a, const BasicAllocation<S>& b) {
	return a.offset != b.offset || a.size != b.size;
}

/// Aligns an offset to the given alignment.
/// An alignment of 0 zero will not change the offset.
/// An offset of 0 is treated as aligned with every possible alignment.
/// Undefined if either value is negative.
template<typename A, typename B>
constexpr auto align(A offset, B alignment) {
	if(offset == 0 || alignment == 0) {
		return offset;
	}

	auto rest = offset % alignment;
	return rest ? A(offset + (alignment - rest)) : A(offset);
}

template<typename A, typename B>
constexpr A alignPOT(A offset, B alignment) {
	dlg_assert(alignment != 0);
	dlg_assert((alignment & (alignment - 1)) == 0u); // POT
	return (offset + alignment - 1) & ~(alignment - 1);
}

/// Returns whether the first allocation fully contains the second one.
template<typename S> constexpr bool
contains(const BasicAllocation<S>& a, const BasicAllocation<S>& b) {
	return (b.offset == std::clamp(b.offset, a.offset, end(a)) &&
		   end(b) == std::clamp(end(b), a.offset, end(a)));
}

// Returns whether the range given by [offA, offA + sizeA) overlaps
// the range given by [offB, offB + sizeB).
constexpr bool overlaps(u32 offA, u32 sizeA, u32 offB, u32 sizeB) {
	auto endA = offA + sizeA;
	auto endB = offB + sizeB;
	return (offA >= offB && offA < endB) ||
		   (offB >= offA && offB < endA);
}

} // namespace vil

