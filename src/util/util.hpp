#pragma once

#include <fwd.hpp>
#include <util/bytes.hpp>
#include <dlg/dlg.hpp>
#include <vk/vulkan.h>
#include <util/vec.hpp>
#include <cstring>
#include <memory>
#include <vector>

namespace vil {

// util
template<typename T>
auto& nonNull(T&& ptr) {
	dlg_assert(ptr);
	return *ptr;
}

template<typename ...Ts>
struct Visitor : Ts...  {
    Visitor(const Ts&... args) : Ts(args)...  {}
    using Ts::operator()...;
};

template <class T>
inline void hash_combine(std::size_t& s, const T & v) {
	s ^= std::hash<T>{}(v) + 0x9e3779b9 + (s<< 6) + (s>> 2);
}

template<typename T>
struct ReversionAdatper {
	T& iterable;

	auto begin() { return std::rbegin(iterable); }
	auto end() { return std::rend(iterable); }
};

template<typename T>
ReversionAdatper<T> reversed(T&& iterable) {
	return {iterable};
}

u32 findLSB(u32);
u32 nextPOT(u32);

/// Does not throw on error, just outputs error.
void writeFile(const char* path, span<const std::byte> buffer, bool binary = true);

template<typename T>
bool isEmpty(const std::weak_ptr<T>& ptr) {
	// https://stackoverflow.com/questions/45507041/how-to-check-if-weak-ptr-is-empty-non-assigned
	using wt = std::weak_ptr<T>;
    return !ptr.owner_before(wt{}) && !wt{}.owner_before(ptr);
}

template<typename... Args> struct LastT;
template<typename Head, typename... Tail> struct LastT<Head, Tail...> : LastT<Tail...> {};
template<typename Head> struct LastT<Head> {
	using type = Head;
};

template<typename... Args> using Last = LastT<Args...>;

// Returns ceil(num / denom), efficiently, only using integer division.
inline constexpr unsigned ceilDivide(unsigned num, unsigned denom) {
	return (num + denom - 1) / denom;
}

// Checks the environment variable with the given name.
// If not set, returns defaultValue.
// If set to 0, returns false. If set to 1, returns true.
// Will output a warning if set to something else and just return
// defaultValue.
bool checkEnvBinary(const char* env, bool defaultValue);

template<typename C, typename K>
auto find(C&& c, K&& k) {
	return std::find(std::begin(c), std::end(c), std::forward<K>(k));
}

template<typename C, typename K>
bool contains(C&& c, K&& k) {
	return find(c, k) != c.end();
}

template<typename C, typename F>
auto find_if(C&& c, F&& f) {
	return std::find_if(std::begin(c), std::end(c), std::forward<F>(f));
}

// C++20: std::erase_if. Impl from cppreference
template<class T, class Alloc, class Pred>
std::size_t erase_if(std::vector<T,Alloc>& c, Pred pred) {
	auto it = std::remove_if(c.begin(), c.end(), pred);
	auto r = std::distance(it, c.end());
	c.erase(it, c.end());
	return r;
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

template<typename CI>
bool hasChain(const CI& ci, VkStructureType sType) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	while(link) {
		if(link->sType == sType) {
			return true;
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return false;
}

template<typename R, VkStructureType SType, typename CI>
const R* findChainInfo(const CI& ci) {
	auto* link = static_cast<const VkBaseInStructure*>(ci.pNext);
	while(link) {
		if(link->sType == SType) {
			return reinterpret_cast<const R*>(link);
		}

		link = static_cast<const VkBaseInStructure*>(link->pNext);
	}

	return nullptr;
}

template<VkStructureType SType>
void* findChainInfo2(void* pNext) {
	auto* link = static_cast<VkBaseOutStructure*>(pNext);
	while(link) {
		if(link->sType == SType) {
			return static_cast<void*>(link);
		}

		link = static_cast<VkBaseOutStructure*>(link->pNext);
	}

	return nullptr;
}

std::unique_ptr<std::byte[]> copyChain(const void*& pNext);
void* copyChain(const void*& pNext, std::unique_ptr<std::byte[]>& buf);

void* copyChainLocal(ThreadMemScope&, const void* pNext);

template<typename T>
auto aliasCmd(T&& list) {
	std::remove_reference_t<decltype(**list.begin())> found = nullptr;
	for(auto& fn : list) {
		if(*fn) {
			found = *fn;
			break;
		}
	}

	if(found) {
		for(auto& fn : list) {
			*fn = *found;
		}
	}

	return found;
}

// Taken from vpp/util
/// Aligns an offset to the given alignment.
/// An alignment of 0 zero will not change the offset.
/// An offset of 0 is treated as aligned with every possible alignment.
/// Undefined if either value is negative.
template<typename A, typename B>
constexpr A align(A offset, B alignment) {
	if(offset == 0 || alignment == 0) {
		return offset;
	}

	auto rest = offset % alignment;
	return rest ? A(offset + (alignment - rest)) : A(offset);
}

// Mainly taken from tkn/formats
VkImageType minImageType(VkExtent3D, unsigned minDim = 1u);
VkImageViewType minImageViewType(VkExtent3D, unsigned layers,
	bool cubemap, unsigned minDim = 1u);

// NOTE: rgb must be linear
u32 e5b9g9r9FromRgb(Vec3f rgb);
Vec3f e5b9g9r9ToRgb(u32 e5r9g9b9);

Vec4d read(VkFormat srcFormat, span<const std::byte>& src);
void write(VkFormat dstFormat, span<std::byte>& dst, const Vec4d& color);
void convert(VkFormat dstFormat, span<std::byte>& dst,
		VkFormat srcFormat, span<const std::byte>& src);

u32 indexSize(VkIndexType type);
u32 readIndex(VkIndexType type, ReadBuf& data);

// does the correct conversion, no pow(2.2) approximation
double linearToSRGB(double linear);
double srgbToLinear(double srgb);

// preserves alpha component, as per vulkan
Vec4d linearToSRGB(Vec4d);
Vec4d srgbToLinear(Vec4d);

std::size_t structSize(VkStructureType type);

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

} // namespace vil
