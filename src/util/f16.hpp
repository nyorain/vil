#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

// Taken from tkn/f16.hpp

namespace vil {

// 16-bit IEEE floating point value, compatible with gpu representations.
// Often called "half".
class f16 final {
public:
	using u16 = std::uint16_t;
	static constexpr unsigned bitsSign = 1;
	static constexpr unsigned bitsExponent = 5;
	static constexpr unsigned bitsMantissa = 10;

public:
	f16() = default;
	f16(float);
	explicit f16(u16 sign, u16 exp, u16 mantissa);

	// TODO: should be explicit.
	// Requires static casting in nytl vec/mat conversion though.
	// Should those be explicit as well?
	operator float() const;

	u16& bits() { return bits_; }
	u16 bits() const { return bits_; }

	unsigned sign() const;
	unsigned exp() const;
	unsigned mantissa() const;

public:
	u16 bits_;
};

bool isnan(f16);
bool isinf(f16);
// TODO: more functions: isfinite, isnormal, fpclassify

inline f16& operator+=(f16& a, f16 b) { return a = float(a) + float(b); }
inline f16& operator-=(f16& a, f16 b) { return a = float(a) - float(b); }
inline f16& operator*=(f16& a, f16 b) { return a = float(a) * float(b); }
inline f16& operator/=(f16& a, f16 b) { return a = float(a) / float(b); }

inline f16& operator+=(f16& a, float b) { return a = float(a) + float(b); }
inline f16& operator-=(f16& a, float b) { return a = float(a) - float(b); }
inline f16& operator*=(f16& a, float b) { return a = float(a) * float(b); }
inline f16& operator/=(f16& a, float b) { return a = float(a) / float(b); }

inline f16 operator+(f16 a, f16 b) { return float(a) + float(b); }
inline f16 operator-(f16 a, f16 b) { return float(a) - float(b); }
inline f16 operator*(f16 a, f16 b) { return float(a) * float(b); }
inline f16 operator/(f16 a, f16 b) { return float(a) / float(b); }

inline f16 operator+(float a, f16 b) { return a + float(b); }
inline f16 operator-(float a, f16 b) { return a - float(b); }
inline f16 operator*(float a, f16 b) { return a * float(b); }
inline f16 operator/(float a, f16 b) { return a / float(b); }

inline f16 operator+(f16 a, float b) { return float(a) + b; }
inline f16 operator-(f16 a, float b) { return float(a) - b; }
inline f16 operator*(f16 a, float b) { return float(a) * b; }
inline f16 operator/(f16 a, float b) { return float(a) / b; }

bool operator==(f16 a, f16 b);
bool operator<=(f16 a, f16 b);
bool operator<(f16 a, f16 b);

inline bool operator!=(f16 a, f16 b) { return !(a == b); }
inline bool operator>(f16 a, f16 b) { return !(a <= b); }
inline bool operator>=(f16 a, f16 b) { return !(a < b); }

inline f16 operator-(f16 a) { a.bits() ^= (1 << 15); return a; }

namespace f16_literal {

inline f16 operator ""_f16(long double val) { return f16(float(val)); }

} // namespace literals
} // namespace vil

namespace std {

template<> class numeric_limits<::vil::f16> {
public:
	static const bool is_specialized = true;
	static vil::f16 min () { return vil::f16(0, 1, 0); }
	static vil::f16 max () { return vil::f16(0, 30, 0xFFFFu); }
	static const int radix = 2;
	static const int digits = 10;
	static const int digits10 = 2;
	static const bool is_signed = true;
	static const bool is_integer = true;
	static const bool is_exact = false;
	static const bool traps = false;
	static const bool is_modulo	= false;
	static const bool is_bounded = true;

	// from https://en.wikipedia.org/wiki/Machine_epsilon
	static vil::f16 epsilon () { return 0.0009765625f; } // 2^-10
	static vil::f16 round_error () { return 0.00048828125; } // 2^-11
	static const int min_exponent10 = -9;
	static const int max_exponent10 = 9;
	static const int min_exponent = -15;
	static const int max_exponent = 15;

	static const bool has_infinity = true;
	static const bool has_quiet_NaN	= true;
	static const bool has_signaling_NaN	= true;
	static const bool is_iec559	= false;
	static const bool has_denorm = denorm_present;
	static const bool tinyness_before = false;
	static const float_round_style round_style = round_to_nearest;

	static vil::f16 denorm_min() { return vil::f16(1, 0, 1); }
	static vil::f16 infinity() { return vil::f16(0, 31, 0); }
	static vil::f16 quiet_NaN() { return vil::f16(0, 31, 1); }
	static vil::f16 signaling_NaN() { return vil::f16(0, 31, 1); }
};

template<> struct is_floating_point<::vil::f16> : std::true_type {};

} // namespace std

