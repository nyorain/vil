#pragma once

#include <complex> // std::complex
#include <iosfwd> // std::ostream
#include <util/util.hpp>

namespace vil {

/// The default epsilon used if no custom epsilon is given.
/// Note that Approx does not use this epsilon hardcoded but will mutiply
/// it with the maximum of the compared values.
constexpr auto defaultApproxEpsilon = 0.00000001;

/// Represents an approximite value of type T.
/// Usually type T is a floating-point value or something related to it, like
/// a fp vector or matrix.
template<typename T>
class Approx;

/// Creates an Approx object for the given value and epsilon.
template<typename T>
Approx<T> approx(const T& value, double epsilon = defaultApproxEpsilon);

/// Default approx implementation for floating point types.
/// Most other specializations use this implementation for their comparisons.
template<typename T>
class Approx {
public:
	static_assert(std::is_floating_point_v<T>,
		"nytl::Approx only works for floating point types");

	friend bool operator==(T lhs, const Approx& rhs) {
		auto max = std::max(std::abs(lhs), std::abs(rhs.value));
		return std::abs(lhs - rhs.value) < rhs.epsilon * (1 + max);
	}

	friend bool operator==(const Approx& lhs, double rhs) {
		return operator==(rhs, lhs);
	}
	friend bool operator!=(double lhs, const Approx& rhs) {
		return !operator==(lhs, rhs);
	}
	friend bool operator!=(const Approx& lhs, double rhs) {
		return !operator==(lhs, rhs);
	}

public:
	T value {};
	double epsilon {defaultApproxEpsilon};
};

/// Approx specialization for std::complex types.
/// Will simply approximate the real and imaginary part separately.
template<typename T>
class Approx<std::complex<T>> {
public:
	template<typename OT>
	friend bool operator==(std::complex<OT> lhs, const Approx& rhs) {
		return lhs.real() == approx(rhs.value.real(), rhs.epsilon)
			&& lhs.imag() == approx(rhs.value.imag(), rhs.epsilon);
	}

	template<typename OT>
	friend bool operator==(const Approx& lhs, std::complex<OT> rhs) {
		return operator==(rhs, lhs);
	}

	template<typename OT>
	friend bool operator!=(std::complex<OT> lhs, const Approx& rhs) {
		return !operator==(lhs, rhs);
	}

	template<typename OT>
	friend bool operator!=(const Approx& lhs, std::complex<OT> rhs) {
		return !operator==(lhs, rhs);
	}

public:
	std::complex<T> value {};
	double epsilon {defaultApproxEpsilon};
};

template<typename T>
Approx<T> approx(const T& value, double epsilon) {
	return {value, epsilon};
}

/// Use this namespace to enable approx printing.
namespace approxOps {

template<typename T>
std::ostream& operator<<(std::ostream& os, const Approx<T>& approx) {
	templatize<T>(os) << "Approx(" << approx.value << ")";
	return os;
}

} // namespace approxOps
} // namespace vil
