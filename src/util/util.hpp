#pragma once

#include <fwd.hpp>
#include <util/dlg.hpp>
#include <vk/vulkan.h>
#include <nytl/vec.hpp>
#include <nytl/bytes.hpp>
#include <nytl/span.hpp>
#include <cstring>
#include <memory>
#include <vector>
#include <mutex>
#include <cctype>

namespace vil {

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
const void* findChainInfo2(const void* pNext) {
	auto* link = static_cast<const VkBaseInStructure*>(pNext);
	while(link) {
		if(link->sType == SType) {
			return static_cast<const void*>(link);
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
		dlg_assert(fn);
		if(*fn) {
			found = *fn;
			break;
		}
	}

	if(found) {
		for(auto& fn : list) {
			if(!*fn) {
				*fn = *found;
			}
		}
	}

	return found;
}

// Mainly taken from tkn/formats
VkImageViewType imageViewForImageType(VkImageType);
VkImageType minImageType(VkExtent3D, unsigned minDim = 1u);
VkImageViewType minImageViewType(VkExtent3D, unsigned layers,
	bool cubemap, unsigned minDim = 1u);

std::size_t structSize(VkStructureType type);

// For shaders/pipelines that access application images, we need
// permutations for the different images types.
struct ShaderImageType {
	// 1,2 dimensional images are usually treated as arrayed as a generalization.
	enum Value {
		u1,
		u2,
		u3,
		i1,
		i2,
		i3,
		f1,
		f2,
		f3,
		count
	};

	static Value parseType(VkImageType type, VkFormat format,
		VkImageAspectFlagBits aspect);
};

// TODO: bad performance due to taking string parameter. Should
// take string_view but then we need to use from_chars which has poor
// support. Should be better in future.
template<typename T>
bool stoi(std::string string, T& val, unsigned base = 10) {
	char* end {};
	auto str = string.c_str();
	auto v = std::strtoll(str, &end, base);
	if(end == str) {
		return false;
	}

	val = v;
	return true;
}

template<typename T>
struct EnumerateImpl {
	using It = decltype(std::declval<T>().begin());
	using Ref = decltype((*std::declval<It>()));

	struct Value {
		std::size_t index;
		Ref item;
	};

 	// Custom iterator with minimal interface
    struct Iterator {
        Iterator(It it, size_t counter = 0) : it_(it), counter_(counter) {}

        Iterator operator++() { return Iterator(++it_, ++counter_); }
        bool operator!=(Iterator other) { return it_ != other.it_; }
        Value operator*() { return Value{counter_, *it_}; }
        Value operator->() { return Value{counter_, *it_}; }

    private:
        It it_;
		std::size_t counter_;
    };

	EnumerateImpl(T& val) : value_(val) {}
	Iterator begin() { return {value_.begin()}; }
	Iterator end() { return {value_.end()}; }

	T& value_;
};

template<typename T>
EnumerateImpl<T> enumerate(T& t) {
    return EnumerateImpl<T>(t);
}

template<typename T>
struct RevertedIteratable {
	T& iterable;
};

template<typename T>
auto begin(RevertedIteratable<T> w) {
	return std::rbegin(w.iterable);
}

template<typename T>
auto end(RevertedIteratable<T> w) {
	return std::rend(w.iterable);
}

template<typename T>
RevertedIteratable<T> reverse(T&& iterable) {
	return {iterable};
}

struct BufferInterval {
	VkDeviceSize offset;
	VkDeviceSize size;
};

BufferInterval minMaxInterval(span<const VkBufferImageCopy2KHR> copies, u32 texelSize);
BufferInterval minMaxInterval(span<const VkBufferCopy2KHR> copies, bool src);

/// Returns the linear mix of x and y with factor a.
/// P must represent a mathematical field.
template<typename P, typename T>
constexpr auto mix(P x, P y, T a) {
	return (1 - a) * x + a * y;
}

// Like a mixture of static_cast and dynamic_cast.
// Will assert (i.e. no-op in release mode) that the given pointer
// can be casted to the requested type and then cast it.
// If the given pointer is null, simply returns null.
// Prefer this (or the variations below) over dynamic_cast or static_cast if
// you are... "sure" that the given base pointer has a type.
// It at least outputs a error in debug mode and has no overhead in release mode.
template<typename T, typename O>
T deriveCast(O* ptr) {
	static_assert(std::is_base_of_v<O, std::remove_pointer_t<T>>);
	dlg_assertt(("deriveCast"), !ptr || dynamic_cast<T>(ptr));
	return static_cast<T>(ptr);
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

// NOTE: this ignores multi-plane aspects, will never return them.
VkImageAspectFlags aspects(VkFormat format);
u32 combineQueueFamilies(span<const u32> queueFams);

template<typename Mutex, typename T>
auto lockCopy(Mutex& mutex, T& obj) {
	std::lock_guard lock(mutex);
	auto cpy = obj;
	return cpy;
}

template<typename T>
std::vector<T> asVector(span<const T> range) {
	return {range.begin(), range.end()};
}

template<typename T, typename O>
int findSubstrCI(const T& haystack, const O& needle) {
	auto cmp = [&](unsigned char c1, unsigned char c2) { return std::tolower(c1) == std::tolower(c2); };
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), cmp);
	return it == haystack.end() ? -1 : it - haystack.begin();
}

} // namespace vil
