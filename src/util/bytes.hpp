#pragma once

#include <nytl/span.hpp>
#include <cstdlib>
#include <type_traits>
#include <vector>
#include <initializer_list>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <util/dlg.hpp>

// taken from nytl/bytes.hpp

namespace vil {

// Define NYTL_BYTES_ASSERT before including this header to define a custom
// assert handler, e.g. like this:
// #define NYTL_BYTES_ASSERT(x) assert(x)
// #define NYTL_BYTES_ASSERT(x) custom_assert(x)
// #define NYTL_BYTES_ASSERT(x) if(!(x)) { throw std::out_of_range(); }
#ifndef NYTL_BYTES_ASSERT
	#define NYTL_BYTES_ASSERT(x) dlg_assert(x)
#endif //  NYTL_BITS_ASSERT

// Converting objects to a readable or writable range of bytes only
// works for objects that have standard layout:
// - all non-static members have same access type
// - no virtual functions
// - no non-static reference members
// - all base classes and non-static members have standard layout
// - no two baseclasses of the same type
// - all non-static data members declared in same class/base
// - [some additional less relevant restrictions on members/bases]
// See https://en.cppreference.com/w/cpp/named_req/StandardLayoutType

// Buffer of fixed size that can be read but not written.
// The data is not owned. Reading will advance the buffer and seeking backwards
// is not possible (skipping section efficiently is, though).
using ReadBuf = span<const std::byte>;

// Buffer of fixed maximum size that can be written.
// Can be transformed into a ReadBuf as well. The data is not owned.
// Writing will advance the buffer and backwards seeking is not possible.
using WriteBuf = span<std::byte>;

// Dynamically resizing write buffer.
using DynWriteBuf = std::vector<std::byte>;

// ProhibitByteConversion can be specialized to prohibit byte conversion
// for types that would otherwise qualify.
template<typename T> struct ProhibitByteConversion :
	public std::false_type {};
template<typename T> struct ProhibitByteConversion<std::initializer_list<T>> :
	public std::true_type {};
template<typename T> struct ProhibitByteConversion<span<T>> :
	public std::true_type {};

template<typename T> constexpr auto BytesConvertible =
	std::is_trivially_copyable_v<T> &&
	std::is_standard_layout_v<T> &&
	!ProhibitByteConversion<T>::value;

// Convert objects to their ReadBuf/WriteBuf representation.
template<typename T>
std::enable_if_t<BytesConvertible<T>, ReadBuf>
bytes(const T& val) {
	return {reinterpret_cast<const std::byte*>(&val), sizeof(val)};
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, WriteBuf>
bytes(T& val) {
	return {reinterpret_cast<std::byte*>(&val), sizeof(val)};
}

template<typename T, std::size_t S>
std::enable_if_t<BytesConvertible<T>, ReadBuf>
bytes(span<const T, S> span) {
	auto ptr = reinterpret_cast<const std::byte*>(span.data());
	return {ptr, span.size() * sizeof(T)};
}

template<typename T, std::size_t S>
std::enable_if_t<BytesConvertible<T>, WriteBuf>
bytes(span<T, S> span) {
	auto ptr = reinterpret_cast<std::byte*>(span.data());
	return {ptr, span.size() * sizeof(T)};
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, ReadBuf>
bytes(const std::vector<T>& val) {
	return nytl::as_bytes(span(val));
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, WriteBuf>
bytes(std::vector<T>& val) {
	return nytl::as_writeable_bytes(span(val));
}

// There is no non-const overload for initializer list since we can't
// ever modify data in it.
template<typename T>
std::enable_if_t<BytesConvertible<T>, ReadBuf>
bytes(const std::initializer_list<T>& val) {
	return nytl::as_bytes(span(val));
}

inline void skip(ReadBuf& buf, std::size_t bytes) {
	NYTL_BYTES_ASSERT(buf.size() >= bytes);
	buf = buf.last(buf.size() - bytes);
}

inline void skip(WriteBuf& buf, std::size_t bytes) {
	NYTL_BYTES_ASSERT(buf.size() >= bytes);
	buf = buf.last(buf.size() - bytes);
}

inline void write(DynWriteBuf& dst, ReadBuf src) {
	dst.resize(dst.size() + src.size());
	auto dstData = dst.data() + dst.size() - src.size();
	std::memcpy(dstData, src.data(), src.size());
}

inline void write(WriteBuf& dst, ReadBuf src) {
	NYTL_BYTES_ASSERT(dst.size() >= src.size());
	std::memcpy(dst.data(), src.data(), src.size());
	dst = dst.last(dst.size() - src.size());
}

template<typename T>
void write(WriteBuf& dst, const T& obj) {
	write(dst, ReadBuf(bytes(obj)));
}

template<typename T>
void write(DynWriteBuf& dst, const T& obj) {
	write(dst, ReadBuf(bytes(obj)));
}

inline void read(ReadBuf& src, WriteBuf dst) {
	NYTL_BYTES_ASSERT(src.size() >= dst.size());
	std::memcpy(dst.data(), src.data(), dst.size());
	src = src.last(src.size() - dst.size());
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, T>
read(ReadBuf& src) {
	T ret;
	read(src, bytes(ret));
	return ret;
}

template<typename T>
std::enable_if_t<BytesConvertible<T>>
read(ReadBuf& src, T& dst) {
	read(src, bytes(dst));
}

// Does not advance any of the buffers
inline void copy(ReadBuf src, WriteBuf dst) {
	NYTL_BYTES_ASSERT(src.size() == dst.size());
	std::memcpy(dst.data(), src.data(), dst.size());
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, T>
copy(ReadBuf src) {
	T ret;
	NYTL_BYTES_ASSERT(src.size() >= sizeof(ret));
	std::memcpy(&ret, src.data(), sizeof(ret));
	return ret;
}

template<typename T>
std::enable_if_t<BytesConvertible<T>, T>
copyExact(ReadBuf src) {
	T ret;
	NYTL_BYTES_ASSERT(src.size() == sizeof(ret));
	std::memcpy(&ret, src.data(), sizeof(ret));
	return ret;
}

// Example for writing a fixed-size data segment:
//
// WriteBuf dst = ...;
// nytl::write(dst, 1.f);
// nytl::write(dst, someStdLayoutStruct);
// nytl::write(dst, nytl::Vec3f{5.f, 6.f, 1.f});
// nytl::write<unsigned>(dst, vectorOfInt.size());
// nytl::write(dst, vectorOfInts);
//
// And reading it afterwards:
//
// ReadBuf src = dst;
// auto f1 = nytl::read<float>(src);
// auto someStdLayoutStruct = nytl::read<...>(src);
// auto vec = nytl::read<nytl::Vec3f>(src);
// auto size = nytl::read<unsigned>(src);
// std::vector<int> vectorOfInt(size);
// nytl::read(src, vectorOfInt);
//
// The symmetry between the write and read apis is quite obvious.
// Cases such as dynamically-sized data must be handled differently though,
// see the reading vectorOfInt example above.
// When instead of a fixed-size data segment, a dynamically resizing
// buffer is desired for writing, just use nytl::DynWriteBuf. Works
// exactly the same and can be converted to ReadBuf as well.

} // namespace vil

