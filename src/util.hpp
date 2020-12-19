#pragma once

#include <fwd.hpp>
#include <bytes.hpp>
#include <dlg/dlg.hpp>
#include <vulkan/vulkan.h>
#include <array>
#include <cstring>

namespace fuen {

// util
template<typename T>
T& nonNull(T* ptr) {
	dlg_assert(ptr);
	return *ptr;
}

template<typename ...Ts>
struct Visitor : Ts...  {
    Visitor(const Ts&... args) : Ts(args)...  {}
    using Ts::operator()...;
};

u32 findLSB(u32 v);

template<typename C, typename K>
auto find(C&& c, K&& k) {
	return std::find(std::begin(c), std::end(c), std::forward<K>(k));
}

template<typename C, typename F>
auto find_if(C&& c, F&& f) {
	return std::find_if(std::begin(c), std::end(c), std::forward<F>(f));
}

// C++20: std::bit_cast
template<typename T, typename O>
auto bit_cast(const O& src) {
	static_assert(BytesConvertible<T> && BytesConvertible<O> && sizeof(T) == sizeof(O));
	T ret;
	std::memcpy(static_cast<void*>(&ret), static_cast<const void*>(&src), sizeof(src));
	return ret;
}

template<typename C>
void ensureSize(C& container, std::size_t size) {
	if(container.size() < size) {
		container.resize(size);
	}
}

template<typename R, VkStructureType SType, typename CI>
const R* findChainInfo(const CI& ci) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	// dlg_trace("pNext chain looking on {} for {}", &ci, vk::name(vk::StructureType(SType)));
	while(link) {
		// dlg_trace("\tpNext: {}", vk::name(vk::StructureType(link->sType)));
		if(link->sType == SType) {
			return reinterpret_cast<const R*>(link);
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return nullptr;
}

// Taken from vpp/util
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

// nytl/vec
template<size_t D, typename T>
class Vec : public std::array<T, D> {
public:
	/// The (static/fixed) size of the type
	static constexpr size_t size() { return D; }

	/// Explicitly casts the Vec to another Vec that may have
	/// a different precision or dimension. Will default construct
	/// any values that cannot be filled (e.g. vec3 -> vec4) or leave
	/// out the last values when the size of vector is shrinked (e.g.
	/// {1, 2, 3} -> {1, 2}).
	template<size_t OD, typename OT>
	constexpr explicit operator Vec<OD, OT>() const {
		auto ret = Vec<OD, OT> {};
		for(auto i = 0u; i < std::min(D, OD); ++i)
			ret[i] = (*this)[i];
		return ret;
	}
};

template<typename... Args>
Vec(Args&&... args) ->
	Vec<sizeof...(Args), std::common_type_t<Args...>>;

template<typename T> using Vec2 = Vec<2, T>;
template<typename T> using Vec3 = Vec<3, T>;
template<typename T> using Vec4 = Vec<4, T>;

using Vec2f = Vec2<float>;
using Vec2i = Vec2<int>;
using Vec2ui = Vec2<unsigned int>;
using Vec2d = Vec2<double>;
using Vec2b = Vec2<bool>;
using Vec2u8 = Vec2<std::uint8_t>;
using Vec2u16 = Vec2<std::uint16_t>;
using Vec2u32 = Vec2<std::uint32_t>;
using Vec2u64 = Vec2<std::uint64_t>;
using Vec2i8 = Vec2<std::int8_t>;
using Vec2i16 = Vec2<std::int16_t>;
using Vec2i32 = Vec2<std::int32_t>;
using Vec2i64 = Vec2<std::int64_t>;

using Vec3f = Vec3<float>;
using Vec3i = Vec3<int>;
using Vec3ui = Vec3<unsigned int>;
using Vec3d = Vec3<double>;
using Vec3c = Vec3<bool>;
using Vec3u8 = Vec3<std::uint8_t>;
using Vec3u16 = Vec3<std::uint16_t>;
using Vec3u32 = Vec3<std::uint32_t>;
using Vec3u64 = Vec3<std::uint64_t>;
using Vec3i8 = Vec3<std::int8_t>;
using Vec3i16 = Vec3<std::int16_t>;
using Vec3i32 = Vec3<std::int32_t>;
using Vec3i64 = Vec3<std::int64_t>;

using Vec4f = Vec4<float>;
using Vec4i = Vec4<int>;
using Vec4ui = Vec4<unsigned int>;
using Vec4d = Vec4<double>;
using Vec4b = Vec4<bool>;
using Vec4u8 = Vec4<std::uint8_t>;
using Vec4u16 = Vec4<std::uint16_t>;
using Vec4u32 = Vec4<std::uint32_t>;
using Vec4u64 = Vec4<std::uint64_t>;
using Vec4i8 = Vec4<std::int8_t>;
using Vec4i16 = Vec4<std::int16_t>;
using Vec4i32 = Vec4<std::int32_t>;
using Vec4i64 = Vec4<std::int64_t>;

// Mainly taken from tkn/formats
bool isDepthFormat(VkFormat);
// bool isStencilFormat(VkFormat);
VkImageType minImageType(VkExtent3D, unsigned minDim = 1u);
VkImageViewType minImageViewType(VkExtent3D, unsigned layers,
	bool cubemap, unsigned minDim = 1u);

// NOTE: rgb must be linear
u32 e5b9g9r9FromRgb(Vec3f rgb);
Vec3f e5b9g9r9ToRgb(u32 e5r9g9b9);

// CPU format conversion. This is needed to support reading and writing of
// data in formats that the GPU does not support.

Vec4d read(VkFormat srcFormat, span<const std::byte>& src);
void write(VkFormat dstFormat, span<std::byte>& dst, const Vec4d& color);
void convert(VkFormat dstFormat, span<std::byte>& dst,
		VkFormat srcFormat, span<const std::byte>& src);

template<typename V, typename T>
decltype(auto) constexpr templatize(T&& value) {
	return std::forward<T>(value);
}

// ValidExpression impl
namespace detail {
template<template<class...> typename E, typename C, typename... T> struct ValidExpressionT {
	static constexpr auto value = false;
};

template<template<class...> typename E, typename... T>
struct ValidExpressionT<E, std::void_t<E<T...>>, T...> {
	static constexpr auto value = true;
};

} // namespace detail

template<template<typename...> typename E, typename... T>
constexpr auto validExpression = detail::ValidExpressionT<E, void, T...>::value;

} // namespace fuen
