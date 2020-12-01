#pragma once

#include "fwd.hpp"
#include "bytes.hpp"
#include <dlg/dlg.hpp>
#include <vulkan/vulkan.h>
#include <vkpp/enums.hpp>
#include <vkpp/structs.hpp>
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
bool isDepthFormat(vk::Format);

// Returns whether the given format an hold high dynamic range data, i.e.
// has a float format and can store values >1.0.
bool isHDR(vk::Format);
bool isSRGB(vk::Format);
vk::Format toggleSRGB(vk::Format);
vk::ImageType minImageType(vk::Extent3D, unsigned minDim = 1u);
vk::ImageViewType minImageViewType(vk::Extent3D, unsigned layers,
	bool cubemap, unsigned minDim = 1u);

// NOTE: rgb must be linear
u32 e5b9g9r9FromRgb(Vec3f rgb);
Vec3f e5b9g9r9ToRgb(u32 e5r9g9b9);

// CPU format conversion. This is needed to support reading and writing of
// data in formats that the GPU does not support.

Vec4d read(vk::Format srcFormat, std::span<const std::byte>& src);
void write(vk::Format dstFormat, std::span<std::byte>& dst, const Vec4d& color);
void convert(vk::Format dstFormat, std::span<std::byte>& dst,
		vk::Format srcFormat, std::span<const std::byte>& src);

} // namespace fuen
