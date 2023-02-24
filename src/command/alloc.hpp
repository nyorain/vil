#pragma once

#include <fwd.hpp>
#include <command/record.hpp>
#include <cb.hpp>

// TODO: this is terrible and only needed because we originally
//   allocated memory from the command buffer and not from the
//   CommandRecord. Need to rewrite all allocations in cb.cpp
//   to directly happen from the record.

namespace vil {

// allocation util
struct CommandAlloc {
	LinAllocator& alloc;

	// implicit constructors
	inline CommandAlloc(LinAllocator& xalloc) : alloc(xalloc) {}
	inline CommandAlloc(CommandRecord& rec) : alloc(rec.alloc) {}
	inline CommandAlloc(CommandBuffer& cb) : alloc(cb.builder().record_->alloc) {}
};

template<typename T, typename... Args>
[[nodiscard]] T& construct(CommandAlloc rec, Args&&... args) {
	auto* raw = rec.alloc.allocate(sizeof(T), alignof(T));
	return *new(raw) T(std::forward<Args>(args)...);
}

template<typename T>
[[nodiscard]] span<T> alloc(CommandAlloc rec, size_t count) {
	return rec.alloc.alloc<T>(count);
}

template<typename T>
[[nodiscard]] T* allocRaw(CommandAlloc rec, size_t count = 1u) {
	if(count == 0) {
		return {};
	}

	auto* raw = rec.alloc.allocate(sizeof(T) * count, alignof(T));
	return new(raw) T[count]();
}

template<typename T>
[[nodiscard]] span<T> allocUndef(CommandAlloc rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = rec.alloc.allocate(sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count];
	return span<T>(arr, count);
}

template<typename T>
[[nodiscard]] T* allocRawUndef(CommandAlloc rec, size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = rec.alloc.allocate(sizeof(T) * count, alignof(T));
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
[[nodiscard]] span<std::remove_const_t<T>> copyEnsureSizeUndef(CommandAlloc ca, span<T> data, size_t minSize) {
	auto count = std::max(data.size(), minSize);
	if(count == 0u) {
		return {};
	}

	auto span = allocUndef<std::remove_const_t<T>>(ca, count);
	std::copy(data.begin(), data.end(), span.data());
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
