// Copyright (c) 2017-2020 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

/// Defines a lightweight POD Vector class, std::array-like.

#pragma once

#ifndef NYTL_INCLUDE_VEC
#define NYTL_INCLUDE_VEC

#include <array> // std::array
#include <algorithm> // std::min
#include <cstdint>
#include <stdexcept>

namespace nytl {

using std::size_t;
template<size_t D, typename T> class Vec;

template<typename T> class Vec<2, T>; // nytl/vec2.hpp
template<typename T> class Vec<3, T>; // nytl/vec3.hpp

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

/// Basic Vector template class.
/// Basically a std::array with vector semantics. There are various operators
/// and utility provided for dealing with it.
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

// - implementation/operators -
// - free operators -
template<size_t D, typename T1, typename T2>
constexpr Vec<D, T1>& operator+=(Vec<D, T1>& a, const Vec<D, T2>& b) {
	for(size_t i = 0; i < D; ++i)
		a[i] += b[i];
	return a;
}

template<size_t D, typename T1, typename T2>
constexpr Vec<D, T1>& operator-=(Vec<D, T1>& a, const Vec<D, T2>& b) {
	for(size_t i = 0; i < D; ++i)
		a[i] -= b[i];
	return a;
}

template<size_t D, typename T, typename F>
constexpr Vec<D, T>& operator*=(Vec<D, T>& vec, const F& fac) {
	for(auto i = 0u; i < D; ++i)
		vec[i] *= fac;
	return vec;
}

template<size_t D, typename T1, typename T2>
constexpr auto operator+(const Vec<D, T1>& a, const Vec<D, T2>& b) {
	Vec<D, decltype(a[0] + b[0])> ret {};
	for(auto i = 0u; i < D; ++i)
		ret[i] = a[i] + b[i];
	return ret;
}

template<size_t D, typename T1, typename T2>
constexpr auto operator-(const Vec<D, T1>& a, const Vec<D, T2>& b) {
	Vec<D, decltype(a[0] - b[0])> ret {};
	for(auto i = 0u; i < D; ++i)
		ret[i] = a[i] - b[i];
	return ret;
}

template<size_t D, typename T>
constexpr auto operator-(Vec<D, T> a) {
	for(auto i = 0u; i < D; ++i)
		a[i] = -a[i];
	return a;
}

template<size_t D, typename T>
constexpr auto operator+(const Vec<D, T>& a) {
	return a;
}

template<size_t D, typename F, typename T>
constexpr auto operator*(const F& f, const Vec<D, T>& a)
		-> Vec<D, decltype(f * a[0])> {
	Vec<D, decltype(f * a[0])> ret {};
	for(auto i = 0u; i < D; ++i)
		ret[i] = f * a[i];
	return ret;
}

template<size_t D, typename T1, typename T2>
constexpr auto operator==(const Vec<D, T1>& a, const Vec<D, T2>& b) {
	for(auto i = 0u; i < D; ++i)
		if(a[i] != b[i])
			return false;
	return true;
}

template<size_t D1, size_t D2, typename T1, typename T2>
constexpr auto operator!=(const Vec<D1, T1>& a, const Vec<D2, T2>& b) {
	return !(a == b);
}

/// Specialization for 2 component Vec.
/// Holds x,y members that are more convenient to access.
/// Compatible with the default class definition.
template<typename T>
class Vec<2, T> {
public:
	T x;
	T y;

public:
	static constexpr size_t size() { return 2; }

	// constexpr Vec() = default;
	// constexpr explicit Vec(const T& all) : x(all), y(all) {}
	// constexpr Vec(const T& x_, const T& y_) : x(x_), y(y_) {}

	constexpr const T* begin() const { return &x; }
	constexpr const T* end() const { return &y + 1; }
	constexpr T* begin() { return &x; }
	constexpr T* end() { return &y + 1; }

	constexpr T& front() { return x; }
	constexpr T& back() { return y; }

	constexpr const T& front() const { return x; }
	constexpr const T& back() const { return y; }

	constexpr T* data() { return &x; }
	constexpr const T* data() const { return &x; }

	// We could use (data()[i]) but this conflicts constexpr (gcc 7).
	// Stl convention is not to check bounds and therefore never throw
	// from operator[] (and so does the default Vec implementation).
	// But we implicitly have to check bounds here and all other alternatives
	// are worse so we throw in the case of out-of-range. It's almost free.
	constexpr T& operator[](size_t i) {
		switch(i) {
			case 0: return x;
			case 1: return y;
			default: throw std::out_of_range("Vec2[]");
		}
	}

	constexpr const T& operator[](size_t i) const {
		switch(i) {
			case 0: return x;
			case 1: return y;
			default: throw std::out_of_range("Vec2[]");
		}
	}

	// implemented in vec.hpp for all specializations
	template<size_t OD, typename OT>
	constexpr explicit operator Vec<OD, OT>() const {
		auto ret = Vec<OD, OT> {};
		for(auto i = 0u; i < std::min(size(), OD); ++i)
			ret[i] = (*this)[i];
		return ret;
	}
};

/// Specialization for 3 component Vec.
/// Holds x,y,z members that are more convenient to access.
/// Compatible with the default class definition.
template<typename T>
class Vec<3, T> {
public:
	T x;
	T y;
	T z;

public:
	static constexpr size_t size() { return 3; }

	// constexpr Vec() = default;
	// constexpr explicit Vec(const T& all) : x(all), y(all), z(all) {}
	// constexpr Vec(const T& x_, const T& y_, const T& z_) : x(x_), y(y_), z(z_) {}

	constexpr const T* begin() const { return &x; }
	constexpr const T* end() const { return &z + 1; }
	constexpr T* begin() { return &x; }
	constexpr T* end() { return &z + 1; }

	constexpr T& front() { return x; }
	constexpr T& back() { return z; }

	constexpr const T& front() const { return x; }
	constexpr const T& back() const { return z; }

	constexpr T* data() { return &x; }
	constexpr const T* data() const { return &x; }

	// See the vec2 implementation for implementation reasoning.
	constexpr T& operator[](size_t i) {
		switch(i) {
			case 0: return x;
			case 1: return y;
			case 2: return z;
			default: throw std::out_of_range("Vec3[]");
		}
	}

	constexpr const T& operator[](size_t i) const {
		switch(i) {
			case 0: return x;
			case 1: return y;
			case 2: return z;
			default: throw std::out_of_range("Vec3[]");
		}
	}

	// implemented in vec.hpp for all specializations
	template<size_t OD, typename OT>
	constexpr explicit operator Vec<OD, OT>() const {
		auto ret = Vec<OD, OT> {};
		for(auto i = 0u; i < std::min(size(), OD); ++i)
			ret[i] = (*this)[i];
		return ret;
	}
};

} // namespace nytl

#endif // header guard

