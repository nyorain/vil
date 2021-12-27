#pragma once

#include <fwd.hpp>
#include <command/record.hpp>
#include <cb.hpp>

namespace vil {

// allocation util
struct CommandAlloc {
	LinAllocator& alloc;

	// implicit constructors
	inline CommandAlloc(LinAllocator& xalloc) : alloc(xalloc) {}
	inline CommandAlloc(CommandRecord& rec) : alloc(rec.alloc) {}
	inline CommandAlloc(CommandBuffer& cb) : alloc(cb.record()->alloc) {}
};

template<typename T, typename... Args>
[[nodiscard]] T& construct(CommandAlloc rec, Args&&... args) {
	auto* raw = vil::allocate(rec.alloc, sizeof(T), alignof(T));
	return *new(raw) T(std::forward<Args>(args)...);
}

template<typename T>
[[nodiscard]] span<T> alloc(CommandAlloc rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = vil::allocate(rec.alloc, sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count]();
	return span<T>(arr, count);
}

template<typename T>
[[nodiscard]] T* allocRaw(CommandAlloc rec, size_t count = 1u) {
	if(count == 0) {
		return {};
	}

	auto* raw = vil::allocate(rec.alloc, sizeof(T) * count, alignof(T));
	return new(raw) T[count]();
}

template<typename T>
[[nodiscard]] span<T> allocUndef(CommandAlloc rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = vil::allocate(rec.alloc, sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count];
	return span<T>(arr, count);
}

template<typename T>
[[nodiscard]] T* allocRawUndef(CommandAlloc rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = vil::allocate(rec.alloc, sizeof(T) * count, alignof(T));
	return new(raw) T[count];
}

[[nodiscard]] inline
const char* copyString(CommandAlloc rec, std::string_view src) {
	auto dst = allocUndef<char>(rec, src.size() + 1);
	std::copy(src.begin(), src.end(), dst.data());
	dst[src.size()] = 0;
	return dst.data();
}

template<typename T>
[[nodiscard]] span<std::remove_const_t<T>> copySpan(CommandAlloc ca, span<T> data) {
	return copySpan(ca, data.data(), data.size());
}

template<typename T>
[[nodiscard]] span<std::remove_const_t<T>> copySpan(CommandAlloc ca, T* data, size_t count) {
	if(count == 0u) {
		return {};
	}

	auto span = allocUndef<std::remove_const_t<T>>(ca, count);
	std::copy(data, data + count, span.data());
	return span;
}

template<typename T>
void ensureSizeUndef(CommandAlloc ca, span<T>& buf, size_t size) {
	if(buf.size() >= size) {
		return;
	}

	auto newBuf = allocUndef<T>(ca, size);
	std::copy(buf.begin(), buf.end(), newBuf.begin());
	buf = newBuf;
}

template<typename T>
void ensureSize(CommandAlloc ca, span<T>& buf, size_t size) {
	if(buf.size() >= size) {
		return;
	}

	auto newBuf = alloc<T>(ca, size);
	std::copy(buf.begin(), buf.end(), newBuf.begin());
	buf = newBuf;
}

} // namespace vil
