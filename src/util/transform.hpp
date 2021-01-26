#pragma once

#include <util/vec.hpp>
#include <util/mat.hpp>
#include <util/vecOps.hpp>
#include <util/matOps.hpp>
#include <util/quaternion.hpp>
#include <cmath>
#include <cassert>

// Taken from tkn/transform

namespace vil {

// Returns a lookAt matrix for the given position and orientation.
// The returned matrix will just move 'pos' into the origin and
// orient everything like the given quaternion.
// Note that this version is independent from the handedness of
// the coordinate system, it just preserves it.
//
// When using this with a camera:
// For a right-handed view space, the camera would by default
// look along the negative z-axis and everything in front of the
// camera would be z < 0 after multiplying this matrix.
// For a left-handed view space, the camera points along the positive
// z axis by default and everything z > 0 is in front of the camera after
// multiplying by this matrix.
template<size_t D = 4, typename P> [[nodiscard]]
nytl::SquareMat<D, P> lookAt(const Quaternion& rot, nytl::Vec3<P> pos) {
	// transpose is same as inverse for rotation matrices
	auto ret = transpose(toMat<4, P>(rot));
	ret[0][3] = -dot(pos, nytl::Vec3f(ret[0]));
	ret[1][3] = -dot(pos, nytl::Vec3f(ret[1]));
	ret[2][3] = -dot(pos, nytl::Vec3f(ret[2]));
	return ret;
}

// Returns a lookAt matrix, that moves and orients the coordinate
// system as specified. Useful to orient cameras or objects.
// - pos: Position of the camera. This point will be mapped
//   to the origin.
// - z: This direction vector will be mapped onto (0, 0, 1), i.e.
//   the positive z-axis. Must be normalized.
//   Most lookAt implementations take
//   a 'center' parameter, i.e. a point towards which the
//   orientation should face.
//   Calculation to make this matrix map into an
//   RH-space: z = normalize(pos - center),
//   LH-space: z = normalize(center - pos).
// - up: A global up vector. Doesn't have to be normalized. This
//   is not necessarily the direction vector that gets mapped
//   to (0, 1, 0), just a reference to create an orthonormal
//   basis from the given z vector. This means it must never be
//   the same as z and must also not be 0.
//
// Given a quaternion q for the other overload of this function,
// lookAt(q, pos) = lookAt(pos, apply(q, {0, 0, 1}), apply(q, {0, 1, 0})).
template<typename P> [[nodiscard]]
nytl::SquareMat<4, P> lookAt(const nytl::Vec3<P>& pos,
		const nytl::Vec3<P>& z, const nytl::Vec3<P>& up) {
	const auto x = normalized(cross(up, z));
	const auto y = cross(z, x); // automatically normalized

	auto ret = nytl::identity<4, P>();

	ret[0] = nytl::Vec4<P>(x);
	ret[1] = nytl::Vec4<P>(y);
	ret[2] = nytl::Vec4<P>(z);

	ret[0][3] = -dot(x, pos);
	ret[1][3] = -dot(y, pos);
	ret[2][3] = -dot(z, pos);

	return ret;
}

} // namespace vil
