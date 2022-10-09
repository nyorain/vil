#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>
#include <nytl/vec.hpp>
#include <nytl/bytes.hpp>

namespace vil {

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

} // namespace vil
