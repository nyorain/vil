// Simple, lightweight and independent Quaternion implementation.
// Mostly put together from snippets and implementation notes on the internet.

#pragma once

#include <cmath> // std::sin
#include <cassert>

#include <util/vec.hpp>
#include <util/vecOps.hpp>
#include <util/mat.hpp>

// What makes quaternions somewhat hard to work with is that once again
// everybody has a different convention regarding roll, pitch, yaw and
// attitude, heading, bank (i.e. the order of rotations, relevant
// to convert from/to euler angles as it is sometimes needed).
// No convention is implicitly assumed here, when creating a quaternion
// from euler angles (rotation sequence) or the other way around,
// the convention has to be specified.

// Just as a reference (but this should not have any impact for this
// header) to inuitively implement rotations in a standard
// graphics coordinate system (e.g. for the camera, where y is up,
// -z is front and x is the right, i.e. right-handed coordinate
// system), the yxz rotation sequence (euler/tait-bryan angles)
// is used.

// Articles about quaternions usually just implicitly assume one of
// the euler-angles rotation sequences when it's needed (without ever
// specifying it). Googling something like "quaternion to euler
// angles" or "euler angles to quaternions" gives many different formulas,
// usually without people specififying for *which* euler rotation
// sequence this formula actually works. Even worse, the association
// between pitch, yaw, roll and the x, y, z axes (which is all quaternions
// know about, there are no inherently attached semantics!) is usually
// implicitly assumed as well.

// Getting euler angles out of quaternions is not trivial, that's why it's
// implemented below. Creating a quaternion out of euler angles can always
// easily be done using the 'axisAngle' constructor and multiplication,
// a yxz rotation by (yaw, pitch, roll) can for instance easily
// be achieved by
// 	Quaternion::axisAngle(0, 1, 0, yaw) *
// 	Quaternion::axisAngle(1, 0, 0, pitch) *
// 	Quaternion::axisAngle(0, 0, 1, roll).

namespace vil {

// fwd
class Quaternion;
[[nodiscard]] inline Quaternion conjugated(const Quaternion& q);
[[nodiscard]] inline Quaternion normalized(const Quaternion& q);

// Represents a mathematical quaternion, useful for representing 3D rotations.
class Quaternion {
public:
	double x {0.f};
	double y {0.f};
	double z {0.f};
	double w {1.f};

public:
	// Constructs a Quaternion from an axis (given by x,y,z) and an angle (in radians)
	// to rotate around the axis.
	[[nodiscard]] static
	Quaternion axisAngle(double ax, double ay, double az, double angle) {
		auto ha = std::sin(angle / 2);
		return {ax * ha, ay * ha, az * ha, std::cos(angle / 2)};
	}

	[[nodiscard]] static
	Quaternion axisAngle(const nytl::Vec3f& axis, double angle) {
		return axisAngle(axis.x, axis.y, axis.z, angle);
	}

	// Creates a quaternion from a yxz rotation sequence, where
	// 'yaw' is the rotation around the y axis, 'pitch' around
	// the x axis and 'roll' around the z axis.
	// Same as
	//   Quaternion::axisAngle(0, 1, 0, yaw) *
	//   Quaternion::axisAngle(1, 0, 0, pitch) *
	//   Quaternion::axisAngle(0, 0, 1, roll).
	// Utility constructor provided for this rotation sequence since we
	// often need it, constructing quaternions for other, custom,
	// rotation sequences can be done with chaining axisAngle
	// rotations as seen above (or by looking up the optimized
	// formula, as done here).
	[[nodiscard]] static
	Quaternion yxz(double yaw, double pitch, double roll) {
		double cy = std::cos(yaw * 0.5);
		double sy = std::sin(yaw * 0.5);
		double cp = std::cos(pitch * 0.5);
		double sp = std::sin(pitch * 0.5);
		double cr = std::cos(roll * 0.5);
		double sr = std::sin(roll * 0.5);

		return {
			cy * sp * cr + sy * cp * sr,
			sy * cp * cr - cy * sp * sr,
			cy * cp * sr - sy * sp * cr,
			cy * cp * cr + sy * sp * sr,
		};
	}

	// Creates a quaternion from an orthogonal 3x3 rotation matrix.
	// Undefined if the given matrix is not orthogonal.
	// Can be used to create a quaternion that transforms the standard
	// base from/to a given (orthogonal) vector base by setting the new
	// base vectors as rows/columns.
	template<typename P> static
	Quaternion fromMat(const nytl::Mat3<P>& m) {
		assert(std::abs(dot(m[0], m[1])) < 0.05);
		assert(std::abs(dot(m[0], m[2])) < 0.05);
		assert(std::abs(dot(m[1], m[2])) < 0.05);

		// d3cw3dd2w32x2b.cloudfront.net/wp-content/uploads/2015/01/matrix-to-quat.pdf
		double t;
		Quaternion q;
		if(m[2][2] < 0) {
			if(m[0][0] > m[1][1]){
				t = 1.0 + m[0][0] - m[1][1] - m[2][2];
				q = {t, m[1][0] + m[0][1], m[0][2] + m[2][0], m[2][1] - m[1][2]};
			} else{
				t= 1.0 - m[0][0] + m[1][1] - m[2][2];
				q = {m[1][0] + m[0][1], t, m[2][1] + m[1][2], m[0][2] - m[2][0]};
			}
		} else {
			if(m[0][0] < -m[1][1]){
				t = 1.0 - m[0][0] - m[1][1] + m[2][2];
				q = {m[0][2] + m[2][0], m[2][1] + m[1][2], t, m[1][0] - m[0][1]};
			} else{
				t = 1.0 + m[0][0] + m[1][1] + m[2][2];
				q = {m[2][1] - m[1][2], m[0][2] - m[2][0], m[1][0] - m[0][1], t};
			}
		}

		double f = 0.5 / std::sqrt(t);
		q.x *= f;
		q.y *= f;
		q.z *= f;
		q.w *= f;
		return q;
	}

public:
	// hamilton product of quaternions
	Quaternion& operator*=(const Quaternion& rhs) {
		auto nx = w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y;
		auto ny = w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x;
		auto nz = w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w;
		auto nw = w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z;
		*this = {nx, ny, nz, nw};
		return *this;
	}
};

// - operators and functions -
inline Quaternion operator*(Quaternion a, const Quaternion& b) {
	return (a *= b);
}

inline Quaternion operator*(double a, Quaternion b) {
	b.x *= a;
	b.y *= a;
	b.z *= a;
	b.w *= a;
	return b;
}

inline Quaternion operator-(Quaternion a, const Quaternion& b) {
	a.x -= b.x;
	a.y -= b.y;
	a.z -= b.z;
	a.w -= b.w;
	return a;
}

inline Quaternion operator+(Quaternion a, const Quaternion& b) {
	a.x += b.x;
	a.y += b.y;
	a.z += b.z;
	a.w += b.w;
	return a;
}

inline bool operator==(const Quaternion& a, const Quaternion& b) {
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

inline bool operator!=(const Quaternion& a, const Quaternion& b) {
	return a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w;
}

// Returns a row-major NxN matrix that represents the given Quaternion.
// Same as an identity matrix with the first 3 colums being
//   apply(q, {1, 0, 0}), apply(q, {0, 1, 0}), apply(q, {0, 0, 1})
template<std::size_t N, typename T = float>
[[nodiscard]] nytl::SquareMat<N, T> toMat(const Quaternion& q) {
	static_assert(N >= 3);
	nytl::SquareMat<N, T> ret {};

	auto wz = q.w * q.z;
	auto wy = q.w * q.y;
	auto wx = q.w * q.x;
	auto xx = q.x * q.x;
	auto xy = q.x * q.y;
	auto xz = q.x * q.z;
	auto yy = q.y * q.y;
	auto yz = q.y * q.z;
	auto zz = q.z * q.z;

	ret[0][0] = 1 - 2 * (yy + zz);
	ret[0][1] = 2 * (xy - wz);
	ret[0][2] = 2 * (wy + xz);

	ret[1][0] = 2 * (xy + wz);
	ret[1][1] = 1 - 2 * (xx + zz);
	ret[1][2] = 2 * (yz - wx);

	ret[2][0] = 2 * (xz - wy);
	ret[2][1] = 2 * (wx + yz);
	ret[2][2] = 1 - 2 * (xx + yy);

	// Make sure the remaining rows/cols are initialized like identiy matrix
	for(auto i = 3u; i < N; ++i) {
		ret[i][i] = 1.f;
	}

	return ret;
}

// Returns the conjugate of the given Quaternion (simply taking the
// negative of the non-real parts).
[[nodiscard]] inline Quaternion conjugated(const Quaternion& q) {
	return {-q.x, -q.y, -q.z, q.w};
}

// Returns the norm of the given Quaternion.
[[nodiscard]] inline double norm(const Quaternion& q) {
	return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
}

// Returns a unit quaternion for the given quaternion.
[[nodiscard]] inline Quaternion normalized(const Quaternion& q) {
	auto l = norm(q);
	if(l <= 0.0) return {0.0, 0.0, 0.0, 1.0};
	return {q.x / l, q.y / l, q.z / l, q.w / l};
}

// Returns the given vector rotated by the rotation represented by the given
// Quaternion.
template<typename T>
[[nodiscard]] nytl::Vec3<T> apply(const Quaternion& q, const nytl::Vec3<T>& v) {
	// optimized version, see
	// https://gamedev.stackexchange.com/questions/28395
	nytl::Vec3d u {q.x, q.y, q.z};
	auto r = 2.0 * dot(u, v) * u
		+ (q.w * q.w - dot(u, u)) * v
		+ 2.0 * q.w * cross(u, v);
	return nytl::Vec3<T>(r);

	// Reference implementation, using the mathematical definition.
	// Same as above but less efficient.
	// auto qv = Quaternion{v.x, v.y, v.z, 0.f};
	// auto qr = (q * qv) * conjugated(q);
	// // assert(std::abs(qr.w) < 0.01f);
	// return {T(qr.x), T(qr.y), T(qr.z)};
}

[[nodiscard]] inline double dot(const Quaternion& a, const Quaternion& b) {
	return a.x * b.x + a.y * b.y + a.z * b.y * a.w * b.y;
}

// https://en.wikipedia.org/wiki/Slerp
// Assumes q0 and q1 to be normalized.
[[nodiscard]] inline Quaternion slerp(Quaternion v0, Quaternion v1, double t) {
    // Compute the cosine of the angle between the two vectors.
    double d = dot(v0, v1);

    // If the dot product is negative, slerp won't take
    // the shorter path. Note that v1 and -v1 are equivalent when
    // the negation is applied to all four components. Fix by
    // reversing one quaternion.
    if (d < 0.0f) {
        v1.x = v1.x;
        v1.y = v1.y;
        v1.z = v1.z;
        v1.w = v1.w;
        d = -d;
    }

    if (d > 0.9995) {
        // If the inputs are too close for comfort, linearly interpolate
        // and normalize the result.
        Quaternion result = v0 + t*(v1 - v0);
        return normalized(result);
    }

    // Since dot is in range [0, DOT_THRESHOLD], acos is safe
    double theta_0 = std::acos(d);   		// theta_0 = angle between input vectors
    double theta = theta_0 * t;          	// theta = angle between v0 and result
    double sin_theta = std::sin(theta);     // compute this value only once
    double sin_theta_0 = std::sin(theta_0); // compute this value only once

    double s0 = cos(theta) - d * sin_theta / sin_theta_0;  // == sin(theta_0 - theta) / sin(theta_0)
    double s1 = sin_theta / sin_theta_0;

    return (s0 * v0) + (s1 * v1);
}

// Sequences of rotation around axes.
// https://en.wikipedia.org/wiki/Euler_angles
enum class RotationSequence {
	// classic euler angles
	xyx,
	xzx,
	yzy,
	yxy,
	zxz,
	zyz,

	// tait-bryan angles
	xyz,
	xzy,
	yxz,
	yzx,
	zxy,
	zyx,
};

// Returns the angles (in radians) for the given rotation sequence
// to reach the orientation of the given quaternion (using intrinsic
// rotation axis definition). For instance, for
// `res = eulerAngles(q, RotationSequence::yxz)`, the rotation
// rotY(res[0]) * rotX(res[1]) * rotZ(res[2]) (for global axes, notice how
// this means global Z rotation is applied first) is the same as
// the given quaternion.
[[nodiscard]] inline std::array<double, 3>
eulerAngles(const Quaternion& q, RotationSequence seq) {
	// TODO: 'indet' handling somewhat hacky atm. We need that value
	// in case the middle rotation is zero (indeterminite case)
	auto classicEuler = [](double a, double b, double c, double d, double e,
			double indet) {
		auto res = std::array<double, 3> {
			std::atan2(d, e),
			std::acos(c),
			std::atan2(a, b),
		};

		if(std::abs(res[1]) < 0.001) {
			// need different handling in this case.
			// (basically middle angle is zero, getting undefined
			// results atm).
			return std::array<double, 3>{std::asin(indet), 0.0, 0.0};
		}

		return res;
	};

	auto taitBryan = [](double a, double b, double c, double d, double e) {
		return std::array<double, 3> {
			std::atan2(a, b),
			std::asin(c),
			std::atan2(d, e),
		};
	};

	switch(seq){
	case RotationSequence::xyx:
		return classicEuler(
			2 * (q.x * q.y + q.w * q.z),
			-2 * (q.x * q.z - q.w * q.y),
			q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z,
			2 * (q.x * q.y - q.w * q.z),
			2 * (q.x * q.z + q.w * q.y),
			2 * (q.w * q.x + q.y * q.z));
	case RotationSequence::xzx:
		return classicEuler(
			2 * (q.x * q.z - q.w * q.y),
			2 * (q.x * q.y + q.w * q.z),
			q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z,
			2 * (q.x * q.z + q.w * q.y),
			-2 * (q.x * q.y - q.w * q.z),
			2 * (q.w * q.x + q.y * q.z));
	case RotationSequence::yxy:
		return classicEuler(
			2 * (q.x * q.y - q.w * q.z),
			2 * (q.y * q.z + q.w * q.x),
			q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z,
			2 * (q.x * q.y + q.w * q.z),
			-2 * (q.y * q.z - q.w * q.x),
			2 * (q.w * q.y + q.x * q.z));
	case RotationSequence::yzy:
		return classicEuler(
			2 * (q.y * q.z + q.w * q.x),
			-2 * (q.x * q.y - q.w * q.z),
			q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z,
			2 * (q.y * q.z - q.w * q.x),
			2 * (q.x * q.y + q.w * q.z),
			2 * (q.w * q.y + q.x * q.z));
	case RotationSequence::zxz:
		return classicEuler(
			2 * (q.x * q.z + q.w * q.y),
			-2 * (q.y * q.z - q.w * q.x),
			q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z,
			2 * (q.x * q.z - q.w * q.y),
			2 * (q.y * q.z + q.w * q.x),
			2 * (q.x * q.y + q.w * q.z));
	case RotationSequence::zyz:
		return classicEuler(
			2 * (q.y * q.z - q.w * q.x),
			2 * (q.x * q.z + q.w * q.y),
			q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z,
			2 * (q.y * q.z + q.w * q.x),
			-2 * (q.x * q.z - q.w * q.y),
			2 * (q.x * q.y + q.w * q.z));

	case RotationSequence::xyz:
		return taitBryan(
			-2 * (q.y * q.z - q.w * q.x),
			q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z,
			2 * (q.x * q.z + q.w * q.y),
		   -2 * (q.x * q.y - q.w * q.z),
			q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z);
	case RotationSequence::xzy:
		return taitBryan(
			2 * (q.y * q.z + q.w * q.x),
			q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z,
			-2 *(q.x * q.y - q.w * q.z),
			2 *(q.x * q.z + q.w * q.y),
			q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z);
	case RotationSequence::yxz:
		return taitBryan(
			2 * (q.x * q.z + q.w * q.y),
			q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z,
			-2 * (q.y * q.z - q.w * q.x),
			2 * (q.x * q.y + q.w * q.z),
			q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z);
	case RotationSequence::yzx:
		return taitBryan(
			-2 * (q.x * q.z - q.w * q.y),
			q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z,
			2 * (q.x * q.y + q.w * q.z),
			-2 * (q.y * q.z - q.w * q.x),
			q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z);
	case RotationSequence::zxy:
		return taitBryan(
			-2 * (q.x * q.y - q.w * q.z),
			q.w * q.w - q.x * q.x + q.y * q.y - q.z * q.z,
			2 * (q.y * q.z + q.w * q.x),
			-2 * (q.x * q.z - q.w * q.y),
			q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z);
	case RotationSequence::zyx:
	  	return taitBryan(
			2 * (q.x * q.y + q.w * q.z),
			q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z,
			-2 * (q.x * q.z - q.w * q.y),
			2 * (q.y * q.z + q.w * q.x),
			q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z);

	default:
		assert(false && "Invalid rotation sequence");
		return {};
   }
}


} // namespace iro

// NOTE: implementation sources:
// - https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles

