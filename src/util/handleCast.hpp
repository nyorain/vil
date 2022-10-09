#pragma once

#include <cstring>
#include <cstdint>
#include <fwd.hpp>

namespace vil {

// TODO(C++20): use std::bit_cast here.
// Should be faster than std::memcpy.

template<typename T>
std::uint64_t handleToU64(T handle) {
	static_assert(sizeof(handle) <= sizeof(std::uint64_t));

#if __GNUC__ >= 11
	return __builtin_bit_cast(std::uint64_t, handle);
#else // __GNUC__
	std::uint64_t id {};
	std::memcpy(&id, &handle, sizeof(handle));
	return id;
#endif // __GNUC__

	// UB, actually gives compilation errors on GCC
	// return *reinterpret_cast<std::uint64_t*>(&handle);
}

template<typename T>
T u64ToHandle(u64 id) {
	static_assert(sizeof(T) <= sizeof(id));

#if __GNUC__ >= 11
	return __builtin_bit_cast(T, id);
#else // __GNUC__
	T ret {};
	std::memcpy(&ret, &id, sizeof(T));
	return ret;
#endif // __GNUC__

	// UB, actually gives compilation errors on GCC
	// return *reinterpret_cast<T*>(&id);
}

} // namespace vil
