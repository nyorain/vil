#pragma once

#include <fwd.hpp>
#include <util/vec.hpp>
#include <util/mat.hpp>
#include <util/vecOps.hpp>
#include <util/matOps.hpp>
#include <util/quaternion.hpp>
#include <cmath>
#include <cassert>

// Taken from tkn/transform

namespace vil {

// Expands the given matrix with ones on the diagonal.
template<size_t D, typename P, size_t O, typename T> [[nodiscard]]
SquareMat<D, P> expandIdentity(const SquareMat<O, T>& m) {
	static_assert(D >= O);
	auto ret = SquareMat<D, P>(m);
	for(auto i = O; i < D; ++i) {
		ret[i][i] = P(1.0);
	}
	return ret;
}

// Homogeneous coordinates.
// Multiply the given transformation matrix with the given
// position (i.e. w = 1) and returns the equivalent 3D vector.
template<typename P> [[nodiscard]]
Vec3<P> multPos(const Mat4<P>& m, Vec3<P> v) {
	auto v4 = m * Vec4<P>{v.x, v.y, v.z, P(1.0)};
	return {v4[0] / v4[3], v4[1] / v4[3], v4[2] / v4[3]};
}

// Given a unit vector, returns two unit vectors that are orthogonal
// to it.
template<typename P> [[nodiscard]]
std::array<Vec3<P>, 2> base(Vec3<P> dir) {
	// good source: DOI 10.1080/2165347X.2012.689606
	// backend.orbit.dtu.dk/ws/portalfiles/portal/126824972/onb_frisvad_jgt2012_v2.pdf

	// [reference implementation, hughes-moeller]
	// auto b2 = (std::abs(dir.x) > std::abs(dir.z)) ?
	// 	Vec3<P> {-dir.y, dir.x, P(0.0)} :
	// 	Vec3<P> {P(0.0), -dir.z, dir.y};
	// normalize(b2);
	// return {cross(b2, dir), b2};

	// [efficient implementation, frisvad]
	if(dir.z < P(-0.99999f)) { // singularity
		return {{
			{P(0.0), P(-1.0), P(0.0)},
			{P(-1.0), P(0.0), P(0.0)}
		}};
	}

	const float a = P(1.0) / (P(1.0) + dir.z);
	const float b = -dir.x * dir.y * a;
	auto b1 = Vec3f{P(1.0) - dir.x * dir.x * a, b, -dir.x};
	auto b2 = Vec3f{b, P(1.0) - dir.y * dir.y * a, -dir.y};
	return {b1, b2};
}

// Returns a matix that scales by s.
template<size_t D = 4, typename P = float, size_t R> [[nodiscard]]
SquareMat<D, P> scaleMat(const Vec<R, P>& s) {
	static_assert(R <= D);
	auto mat = identity<D, float>();
	for(auto i = 0u; i < R; ++i) {
		mat[i][i] *= s[i];
	}

	return mat;
}

// Returns a matix that translates by t.
template<size_t D = 4, typename P = float, size_t R> [[nodiscard]]
SquareMat<D, P> translateMat(const Vec<R, P>& t) {
	static_assert(R <= D);
	auto mat = identity<D, float>();
	for(auto i = 0u; i < R; ++i) {
		mat[i][D - 1] = t[i];
	}

	return mat;
}

// For all rotations: prefer functions that don't use any angles
// if possible. Use lookAt or orientMat variants if that
// is what you want instead of computing axis and angle.
// See https://www.iquilezles.org/www/articles/noacos/noacos.htm

// Returns a matrix that rotates by rot (in 2 dimensions).
template<size_t D = 4, typename P = float> [[nodiscard]]
SquareMat<D, P> rotateMat(P rot) {
	const auto c = std::cos(rot);
	const auto s = std::sin(rot);
	return expandIdentity<D, P>(Mat2<P>{
		c, -s,
		s, c,
	});
}

// Returns a 2D rotation matrix that maps 'from' to 'to'.
// When 'from' or 'to' are not unit vectors, will also accord for the scale.
template<size_t D = 4, typename P = float> [[nodiscard]]
SquareMat<D, P> orientMat(Vec2<P> from, Vec2<P> to) {
	const auto c = dot(from, to);
	const auto s = cross(from, to);
	return expandIdentity<D, P>(Mat2<P>{
		c, -s,
		s, c,
	});
}

// Uses Rodrigues' rotation formula to rotate the given vector 'v'
// by 'angle' (in radians) around the given 'axis'.
// Expects 'axis' to be a unit vector.
template<typename P> [[nodiscard]]
Vec3<P> rotate(Vec3<P> v, Vec3<P> axis, P angle) {
	// https://en.wikipedia.org/wiki/Rodrigues%27_rotation_formula
	const auto c = std::cos(angle);
	return c * v
		+ std::sin(angle) * cross(axis, v)
		+ dot(axis, v) * (P(1.0) - c) * axis;
}

// Returns a matrix that rotates by the given angle (in radians)
// around the given axis vector 'r'. Expects 'axis' to be normalized.
// NOTE: to make sure a given vector is mapped onto another vector
//  by a rotation matrix, prefer to use orientMat instead of manually
//  determining axis and angle.
template<size_t D = 4, typename P> [[nodiscard]]
SquareMat<D, P> rotateMat(const Vec3<P>& r, P angle) {
	const auto c = std::cos(angle);
	const auto s = std::sin(angle);
	const auto t = (P(1.0) - c) * r;
	return expandIdentity<D, P>(Mat3<P>{
		t.x * r.x + c,        t.x * r.y - s * r.z,  t.x * r.z + s * r.y,
		t.y * r.x + s * r.z,  t.y * r.y + c,        t.y * r.z - s * r.x,
		t.z * r.x - s * r.y,  t.z * r.y + s * r.x,  t.z * r.z + c,
	});
}

// Returns a rotation matrix that maps 'from' to 'to'.
// Expects both vectors to be normalized, cannot correctly account for scale
// (this can be achieved by just normalizing the vectors and scaling
// afterwards).
// NOTE: does not work when 'from' and 'to' are the same vector in opposite
// directions. In that case the rotation is not unique.
// NOTE: the differences between this (with from = dir and
// to = (0, 0, 1)) and lookAt:
//  - lookAt additionally supports a translation
//  - lookAt allows more explicit control of the roll via the 'up'
//    parameter. For a fixed 'up', the returned matrix will always
//    have the same rotation around 'dir'. With this implementation,
//    no explicit control is possible and all axes will just be
//    rotated consistently for the given 'from' and 'to'.
//    So in cases where you don't want roll, lookAt is the
//    better choice.
// NOTE: basically the same as rotateMat, with the rotation axis
// being `normalized(cross(from, to))` and angle being `acos(dot(from, to))`.
// But this implementation is cleaner and faster.
template<size_t D = 4, typename P> [[nodiscard]]
SquareMat<D, P> orientMat(Vec3<P> from, Vec3<P> to) {
	const auto s = cross(from, to); // rotation axis, length: sin(angle)
	const auto c = dot(from, to); // cos(angle)
	const auto k = P(1.0) / (P(1.0) + c);
	return expandIdentity<D, P>(Mat3<P>{
		s.x * s.x * k + c,    s.y * s.x * k - s.z,  s.z * s.x * k + s.y,
		s.x * s.y * k + s.z,  s.y * s.y * k + c,    s.z * s.y * k - s.x,
		s.x * s.z * k - s.y,  s.y * s.z * k + s.x,  s.z * s.z * k + c,
	});
}

template<size_t D, typename P>
void rotate(SquareMat<D, P>& mat, P rot) {
	mat = rotateMat<D, P>(rot) * mat;
}

template<size_t D, typename P>
void rotate(SquareMat<D, P>& mat, const Vec3<P>& vec, P angle) {
	mat = rotateMat<D, P>(vec, angle) * mat;
}

template<size_t D, typename P, size_t R>
void translate(SquareMat<D, P>& mat, const Vec<R, P>& t) {
	mat = translateMat<D, P>(t) * mat;
}

template<size_t D, typename P, size_t R>
void scale(SquareMat<D, P>& mat, const Vec<R, P>& s) {
	mat = scaleMat<D, P>(s) * mat;
}

// Flips the y-row of the given matrix.
// `flipAxis<N>(mat)` is equivalent to `mat = M * mat` where M is the
// identity matrix, except that the Nth row has a '-1' on the diagonal.
// After calling this, the given matrix will just output negated coordinates
// for axis N.
template<size_t Axis, size_t D, typename P>
void flipAxis(SquareMat<D, P>& mat) {
	static_assert(Axis < D);
	mat[Axis] = -mat[Axis];
}

template<size_t Axis, size_t D, typename P>
SquareMat<D, P> flippedAxis(SquareMat<D, P> mat) {
	flipAxis<Axis>(mat);
	return mat;
}

template<size_t D, typename P>
void flipY(SquareMat<D, P>& mat) {
	return flipAxis<1>(mat);
}

// Useful to flip the viewport (e.g. for vulkan) like this:
// flippedY(perspective(fov, aspect, near, far)).
template<size_t D, typename P> [[nodiscard]]
SquareMat<D, P> flippedY(SquareMat<D, P> mat) {
	return flippedAxis<1>(mat);
}

template<size_t D, typename P>
void flipZ(SquareMat<D, P>& mat) {
	return flipAxis<2>(mat);
}

template<size_t D, typename P>
SquareMat<D, P> flippedZ(SquareMat<D, P> mat) {
	return flippedAxis<2>(mat);
}

// Builds an orthographic projection matrix.
// Projects the axis-aligned box described by the given outlining
// coordinates into the [-1, 1] x [-1, 1] x [0, 1] vulkan clip-space cube.
// 'near' and 'far' can be any values basically. For a standard right-handed
// orthographic projection (near plane to depth = 0 and far plane to depth = 1),
// they would both be negative and the absolute value of far greater than
// near (so e.g. near = -0.1, far = -100.0).
// Making both positive (but keeping the abs(far) > abs(near)) results
// in a left-handed interpretation of the projected coordinates, i.e.
// positive z-values will be projected into the [0, 1] depth range.
// When abs(near) > abs(far), the depth buffer is basically reversed
// (still mapping near to 0, far to 1).
template<typename P> [[nodiscard]]
SquareMat<4, P> ortho(P left, P right, P bot, P top, P near, P far) {
	auto ret = Mat4f {};
	ret[0][0] = P(2) / (right - left);
	ret[1][1] = P(2) / (top - bot);
	ret[2][2] = P(1) / (far - near);
	ret[3][3] = 1;

	ret[0][3] = (right + left) / (left - right);
	ret[1][3] = (top + bot) / (bot - top);
	ret[2][3] = near / (near - far);
	return ret;
}

// Good general resource on perspective depth-buffer mapping:
// https://developer.nvidia.com/content/depth-precision-visualized
// Basically concludes that reversing the depth buffer is a good idea
// and setting the far plane to infinity isn't a problem then.

// Returns a perspective projection matrix that maps the frustum specified by the
// given parameters into vulkan ndc space (x and y in [-1, 1], z in [0, 1]).
// For instance, the returns matrix maps
//   (left, bot, near) to (-1, -1, 0)
//   (right, top, far) to (1, 1, 1)
// You can imagine left/right,top/bot as being the min/max x,y coordinates
// of the frustum at the near plane.
// NOTE: compared to glFrustum and functions derived from it (GLM for instance),
// near and far can be positive or negative here and are not implicitly
// mirrored. To get behavior as in glm, just pass the respective negative
// near and far value and leave the other values.
// Just to avoid confusion: the returned matrix is different but produces
// equivalent vectors in homogeneous coordinates, since
// (x, y, z w) ~ (-x, -y, -z, -w); they are basically the same.
// NOTE: Passing negative near/far values represents a RH coordinate system,
// positive values a LH coordinate system.
// The glm implementation frustum seems weird for LH,
// it does not flip the translation values; the first two entries in column 3.
// Our implementation should match the D3DXMatrixPerspectiveOffCenter{RH, LH}
// implementations.
// NOTE: for restricted functions that are less explicit, see
// a 'perspective' variant
template<typename P> [[nodiscard]]
Mat4<P> frustum(P left, P right, P bot, P top, P near, P far) {
	assert((near != 0) && "near must not be zero");
	assert(std::abs(far) > std::abs(near) && "far must be behind near plane");
	assert((near >= 0) == (far >= 0) && "near, far must have the same sign");
	assert((left != right) && (bot != top));

	auto ret = Mat4f {};

	ret[0][0] = P(2) * near / (right - left);
	ret[1][1] = P(2) * near / (top - bot);
	ret[2][2] = far / (far - near);

	ret[0][2] = (right + left) / (left - right);
	ret[1][2] = (top + bot) / (bot - top);
	ret[3][2] = 1;

	ret[2][3] = -(far * near) / (far - near);
	return ret;
}

// Like frustum but maps values on the near plane to z = 1 and
// values on the far plane to z = 0 (i.e. reverses the depth buffer).
template<typename P> [[nodiscard]]
Mat4<P> frustumRev(P left, P right, P bot, P top, P near, P far) {
	assert((near != 0) && "near must not be zero");
	assert(std::abs(far) > std::abs(near) && "far must be behind near plane");
	assert((near >= 0) == (far >= 0) && "near, far must have the same sign");
	assert((left != right) && (bot != top));

	auto ret = Mat4f {};

	ret[0][0] = P(2) * near / (right - left);
	ret[1][1] = P(2) * near / (top - bot);
	ret[2][2] = -near / (far - near);

	ret[0][2] = (right + left) / (left - right);
	ret[1][2] = (top + bot) / (bot - top);
	ret[3][2] = 1;

	ret[2][3] = (far * near) / (far - near);
	return ret;
}

// Like frustumRev but has the far plane at (positive or negative, depending
// on the sign of 'near') infinity basically.
// It's important to use a reversed depth buffer for this since the
// precision of floating point numbers near 0 is greater than near
// 1, we need that for high z-values (that get projected to depth
// values that are almost 0).
template<typename P> [[nodiscard]]
Mat4<P> frustumRevInf(P left, P right, P bot, P top, P near) {
	assert((near != 0) && "near must not be zero");
	assert((left != right) && (bot != top));

	auto ret = Mat4f {};

	ret[0][0] = P(2) * near / (right - left);
	ret[1][1] = P(2) * near / (top - bot);

	ret[0][2] = (right + left) / (left - right);
	ret[1][2] = (top + bot) / (bot - top);
	ret[3][2] = 1;

	ret[2][3] = near;
	return ret;
}

// Returns a perspective projection matrix.
// - fov: visibility angle in radians on y-axis
// - aspect: width/height ratio. Usually used to account for non-square
//   output targets. Should be >0. When this is >1, the fov along
//   the x axis will be greater than the specified fov, for aspect < 1
//   smaller.
// - near, far: z-value of the near and far planes, respectively.
//   Will always map z = near to depth = 0 and z = far to depth = 1.
//   Both values must have the same sign (otherwise the projection
//   from the origin does not make sense). But unlike in many
//   OpenGL-near implementations, the sign won't implicitly
//   be flipped, if negative z-values should be projected into
//   the resulting [0, 1] depth space, near and far must be negative.
//   Negative near, far means RH (right-handed) interpreation of the
//   projected coordinates, positive values LH interpreation.
//   To get a reversed depth buffer, near and far can be swapped.
template<typename P = float> [[nodiscard]]
SquareMat<4, P> perspective(P fov, P aspect, P near, P far) {
	assert((near != 0) && "near must not be zero");
	assert((near != far) && "near and far must not be the same value");
	assert((near >= 0) == (far >= 0) && "near, far must have the same sign");

	P const a = P(1.0) / std::tan(fov / P(2.0));

	auto ret = Mat4f {};
	ret[0][0] = a / aspect;
	ret[1][1] = a;

	auto s = far > P(0.0) ? P(1.0) : P(-1.0);
	ret[2][2] = s * far / (far - near);
	ret[3][2] = s;

	ret[2][3] = -s * (far * near) / (far - near);
	return ret;
}

// Like 'perspective' but reversed the depth buffer, i.e. maps
// z = near to depth = 1 and z = far to depth = 0.
// Just a shortcut for reversing the two parameters.
template<typename P = float> [[nodiscard]]
SquareMat<4, P> perspectiveRev(P fov, P aspect, P near, P far) {
	return perspective(fov, aspect, far, near);
}

// Like 'perspective' but reversed the depth buffer, i.e.
// maps z = near to depth = 1. Furthermore sets the far plane
// to infinity (positive or negative depending on the sign of 'near').
// Reversing the depth buffer for this operation is important since
// the precision of floating point values near 0 is significantly
// better than near 1.
template<typename P = float> [[nodiscard]]
SquareMat<4, P> perspectiveRevInf(P fov, P aspect, P near) {
	assert((near != 0) && "near must not be zero");

	P const a = P(1) / std::tan(fov / P(2));
	auto ret = Mat4f {};
	ret[0][0] = a / aspect;
	ret[1][1] = a;

	auto s = near > P(0.0) ? P(1.0) : P(-1.0);
	ret[3][2] = s;
	ret[2][3] = s * near;
	return ret;
}

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
SquareMat<D, P> lookAt(const Quaternion& rot, Vec3<P> pos) {
	// transpose is same as inverse for rotation matrices
	auto ret = transpose(toMat<4, P>(rot));
	ret[0][3] = -dot(pos, Vec3f(ret[0]));
	ret[1][3] = -dot(pos, Vec3f(ret[1]));
	ret[2][3] = -dot(pos, Vec3f(ret[2]));
	return ret;

	// reference implementation, same result but slower
	// auto ret = Mat4f {};
	// auto x = normalized(apply(rot, Vec3f {1.f, 0.f, 0.f}));
	// auto y = normalized(apply(rot, Vec3f {0.f, 1.f, 0.f}));
	// auto z = normalized(apply(rot, Vec3f {0.f, 0.f, 1.f}));
	// ret[0] = Vec4f(x);
	// ret[1] = Vec4f(y);
	// ret[2] = Vec4f(z);
	// ret[0][3] = -dot(pos, x);
	// ret[1][3] = -dot(pos, y);
	// ret[2][3] = -dot(pos, z);
	// ret[3][3] = 1.f;
	// return ret;
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
SquareMat<4, P> lookAt(const Vec3<P>& pos,
		const Vec3<P>& z, const Vec3<P>& up) {
	const auto x = normalized(cross(up, z));
	const auto y = cross(z, x); // automatically normalized

	auto ret = identity<4, P>();

	ret[0] = Vec4<P>(x);
	ret[1] = Vec4<P>(y);
	ret[2] = Vec4<P>(z);

	ret[0][3] = -dot(x, pos);
	ret[1][3] = -dot(y, pos);
	ret[2][3] = -dot(z, pos);

	return ret;
}

// Like the lookAt overload above but does not include a translation.
// Can therefore fit the transformation into a 3x3 matrix.
template<size_t D, typename P> [[nodiscard]]
SquareMat<D, P> lookAt(const Vec3<P>& z, const Vec3<P>& up) {
	static_assert(D >= 3);

	const auto x = normalized(cross(up, z));
	const auto y = cross(z, x); // automatically normalized

	auto ret = identity<D, P>();

	ret[0] = Vec<D, P>(x);
	ret[1] = Vec<D, P>(y);
	ret[2] = Vec<D, P>(z);
	return ret;
}

} // namespace vil
