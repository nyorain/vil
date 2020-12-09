#include <util.hpp>
#include <bytes.hpp>
#include <f16.hpp>
#include <dlg/dlg.hpp>
#include <cmath>

// Intentionally including cpp file
#include <vulkan/vk_format_utils.cpp>

namespace fuen {

// high-level stuff
u32 findLSB(u32 v) {
	// https://stackoverflow.com/questions/757059
	static const int blackMagic[32] = {
		0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
		31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
	};
	return blackMagic[((u32)((v & -v) * 0x077CB531U)) >> 27];
}

bool isDepthFormat(VkFormat format) {
	switch(format) {
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D16_UNORM_S8_UINT:
			return true;
		default:
			return false;
	}
}

template<std::size_t N, typename T>
Vec<N, T> read(span<const std::byte>& src) {
	Vec<N, T> ret;
	for(auto& val : ret) {
		read(src, val);
	}
	return ret;
}

Vec4d read(VkFormat srcFormat, span<const std::byte>& src) {
	switch(srcFormat) {
		case VK_FORMAT_R16_SFLOAT:
			return Vec4d(read<1, f16>(src));
		case VK_FORMAT_R16G16_SFLOAT:
			return Vec4d(read<2, f16>(src));
		case VK_FORMAT_R16G16B16_SFLOAT:
			return Vec4d(read<3, f16>(src));
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			return Vec4d(read<4, f16>(src));

		case VK_FORMAT_R32_SFLOAT:
			return Vec4d(read<1, float>(src));
		case VK_FORMAT_R32G32_SFLOAT:
			return Vec4d(read<2, float>(src));
		case VK_FORMAT_R32G32B32_SFLOAT:
			return Vec4d(read<3, float>(src));
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			return Vec4d(read<4, float>(src));

		case VK_FORMAT_R64_SFLOAT:
			return Vec4d(read<1, double>(src));
		case VK_FORMAT_R64G64_SFLOAT:
			return Vec4d(read<2, double>(src));
		case VK_FORMAT_R64G64B64_SFLOAT:
			return Vec4d(read<3, double>(src));
		case VK_FORMAT_R64G64B64A64_SFLOAT:
			return Vec4d(read<4, double>(src));

		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			return Vec4d(e5b9g9r9ToRgb(read<u32>(src)));

		default:
			throw std::logic_error("Format not supported for CPU reading");
	}
}

void write(VkFormat dstFormat, span<std::byte>& dst, const Vec4d& color) {
	switch(dstFormat) {
		case VK_FORMAT_R32_SFLOAT:
			write(dst, float(color[0]));
			break;
		case VK_FORMAT_R32G32_SFLOAT:
			write(dst, Vec2f(color));
			break;
		case VK_FORMAT_R32G32B32_SFLOAT:
			write(dst, Vec3f(color));
			break;
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			write(dst, Vec4f(color));
			break;

		case VK_FORMAT_R64_SFLOAT:
			write(dst, double(color[0]));
			break;
		case VK_FORMAT_R64G64_SFLOAT:
			write(dst, Vec2d(color));
			break;
		case VK_FORMAT_R64G64B64_SFLOAT:
			write(dst, Vec3d(color));
			break;
		case VK_FORMAT_R64G64B64A64_SFLOAT:
			write(dst, Vec4d(color));
			break;

		case VK_FORMAT_R16_SFLOAT:
			write(dst, f16(color[0]));
			break;
		case VK_FORMAT_R16G16_SFLOAT:
			write(dst, Vec2<f16>(color));
			break;
		case VK_FORMAT_R16G16B16_SFLOAT:
			write(dst, Vec3<f16>(color));
			break;
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			write(dst, Vec4<f16>(color));
			break;

		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			write(dst, e5b9g9r9FromRgb(Vec3f(color)));
			break;

		default:
			throw std::logic_error("Format not supported for CPU writing");
	}
}

void convert(VkFormat dstFormat, span<std::byte>& dst,
		VkFormat srcFormat, span<const std::byte>& src) {
	auto col = read(srcFormat, src);
	write(dstFormat, dst, col);
}

// Implementation directly from the OpenGL EXT_texture_shared_exponent spec
// https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/
//  d62c37dde0a40148aecc9e9701ba0ae4ab83ee22/extensions/EXT/
//  EXT_texture_shared_exponent.txt
// Notable differences: we use an endianess-agnostic implementation that
// extracts bit parts manually. Might hurt performance marginally but makes
// the implementation simpler. Also use already existent modern utility.
namespace e5b9g9r9 {
	constexpr auto expBias = 15;
	constexpr auto maxBiasedExp = 32;
	constexpr auto maxExp = maxBiasedExp - expBias;
	constexpr auto mantissaValues = 1 << 9;
	constexpr auto maxMantissa = mantissaValues - 1;
	constexpr auto max = float(maxMantissa) / mantissaValues * (1 << maxExp);

	float clamp(float x) {
		// x == NaN fails first comparison and returns 0.0
		// That's why we don't use std::clamp
		return x > 0.0 ? ((x > max) ? max : x) : 0.0;
	}

	int floorLog2(float x) {
		// int res;
		// std::frexp(x, &res);
		// return res;

		// Ok, FloorLog2 is not correct for the denorm and zero values, but we
		// are going to do a max of this value with the minimum rgb9e5 exponent
		// that will hide these problem cases.
		u32 uval;
		static_assert(sizeof(x) == sizeof(uval));
		std::memcpy(&uval, &x, sizeof(x));
		return int((uval >> 23) & 0b11111111u) - 127;
	}

} // namespace e5r9g9b9


u32 e5b9g9r9FromRgb(Vec3f rgb) {
	using namespace e5b9g9r9;
	auto rc = clamp(rgb[0]);
	auto gc = clamp(rgb[1]);
	auto bc = clamp(rgb[2]);
	auto maxrgb = std::max(rc, std::max(gc, bc));

	int expShared = std::max(0, floorLog2(maxrgb) + 1 + expBias);
	dlg_assert(expShared <= maxBiasedExp);
	dlg_assert(expShared >= 0);

	/* This pow function could be replaced by a table. */
	double denom = std::exp2(expShared - expBias - 9);
	int maxm = (int) std::floor(maxrgb / denom + 0.5);
	if(maxm == maxMantissa + 1) {
		denom *= 2;
		expShared += 1;
		dlg_assert(expShared <= maxBiasedExp);
	} else {
		dlg_assert(maxm <= maxMantissa);
	}

	auto rm = (int) std::floor(rc / denom + 0.5);
	auto gm = (int) std::floor(gc / denom + 0.5);
	auto bm = (int) std::floor(bc / denom + 0.5);

	dlg_assert(rm <= maxMantissa);
	dlg_assert(gm <= maxMantissa);
	dlg_assert(bm <= maxMantissa);
	dlg_assert(rm >= 0);
	dlg_assert(gm >= 0);
	dlg_assert(bm >= 0);

	return (expShared << 27) | (bm << 18) | (gm << 9) | rm;
}

Vec3f e5b9g9r9ToRgb(u32 ebgr) {
	using namespace e5b9g9r9;

	int exponent = int(ebgr >> 27) - int(expBias) - 9u;
  	float scale = (float) pow(2, exponent);
	return {
		scale * (ebgr & 0b111111111u),
		scale * ((ebgr >> 9) & 0b111111111u),
		scale * ((ebgr >> 18) & 0b111111111u),
	};
}

} // namespace fuen
