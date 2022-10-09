#pragma once

#include <fwd.hpp>
#include <nytl/vec.hpp>
#include <nytl/quaternion.hpp>
#include <nytl/transform.hpp>

// Taken from tkn/camera

namespace vil {

struct Camera {
	Vec3f pos {0.f, 0.f, 1.f}; // position of camera in world space
	Quaternion rot {}; // transforms from camera/view space to world space
};

[[nodiscard]] inline nytl::Vec3f dir(const Camera& c) {
	return -apply(c.rot, nytl::Vec3f {0.f, 0.f, 1.f});
}

[[nodiscard]] inline nytl::Vec3f up(const Camera& c) {
	return apply(c.rot, nytl::Vec3f {0.f, 1.f, 0.f});
}

[[nodiscard]] inline nytl::Vec3f right(const Camera& c) {
	return apply(c.rot, nytl::Vec3f {1.f, 0.f, 0.f});
}

[[nodiscard]] inline nytl::Mat4f viewMatrix(const Camera& c) {
	return lookAt(c.rot, c.pos);
}

} // namespace vil
