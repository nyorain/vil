#include "util.hpp"
#include "bytes.hpp"
#include "f16.hpp"
#include <dlg/dlg.hpp>
#include <cmath>

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

bool isDepthFormat(vk::Format format) {
	switch(format) {
		case vk::Format::d32Sfloat:
		case vk::Format::d32SfloatS8Uint:
		case vk::Format::d24UnormS8Uint:
		case vk::Format::d16Unorm:
		case vk::Format::d16UnormS8Uint:
			return true;
		default:
			return false;
	}
}

bool isHDR(vk::Format format) {
	// TODO: not sure about scaled formats, what are those?
	//  also what about packed formats? e.g. vk::Format::b10g11r11UfloatPack32?
	// TODO: even for snorm/unorm 16/32 bit formats we probably want to
	//  use the stbi hdr loader since otherwise we lose the precision
	//  when stbi converts to 8bit
	switch(format) {
		case vk::Format::r16Sfloat:
		case vk::Format::r16g16Sfloat:
		case vk::Format::r16g16b16Sfloat:
		case vk::Format::r16g16b16a16Sfloat:
		case vk::Format::r32Sfloat:
		case vk::Format::r32g32Sfloat:
		case vk::Format::r32g32b32Sfloat:
		case vk::Format::r32g32b32a32Sfloat:
		case vk::Format::r64Sfloat:
		case vk::Format::r64g64Sfloat:
		case vk::Format::r64g64b64Sfloat:
		case vk::Format::r64g64b64a64Sfloat:
		// NOTE: we consider this format hdr since it can represent numbers
		// >1.0 and can have almost floating point precision
		case vk::Format::e5b9g9r9UfloatPack32:
			return true;
		default:
			return false;
	}
}

bool isSRGB(vk::Format format) {
	switch(format) {
		case vk::Format::r8Srgb:
		case vk::Format::r8g8Srgb:
		case vk::Format::r8g8b8Srgb:
		case vk::Format::r8g8b8a8Srgb:
		case vk::Format::b8g8r8a8Srgb:
		case vk::Format::b8g8r8Srgb:
		case vk::Format::a8b8g8r8SrgbPack32:
			return true;
		default:
			return false;
	}
}

vk::Format toggleSRGB(vk::Format format) {
	switch(format) {
		case vk::Format::r8Srgb:
			return vk::Format::r8Unorm;
		case vk::Format::r8g8Srgb:
			return vk::Format::r8g8Unorm;
		case vk::Format::r8g8b8Srgb:
			return vk::Format::r8g8b8Unorm;
		case vk::Format::r8g8b8a8Srgb:
			return vk::Format::r8g8b8a8Unorm;
		case vk::Format::b8g8r8a8Srgb:
			return vk::Format::b8g8r8a8Unorm;
		case vk::Format::b8g8r8Srgb:
			return vk::Format::b8g8r8Unorm;
		case vk::Format::a8b8g8r8SrgbPack32:
			return vk::Format::a8b8g8r8UnormPack32;

		case vk::Format::r8Unorm:
			return vk::Format::r8Srgb;
		case vk::Format::r8g8Unorm:
			return vk::Format::r8g8Srgb;
		case vk::Format::r8g8b8Unorm:
			return vk::Format::r8g8b8Srgb;
		case vk::Format::r8g8b8a8Unorm:
			return vk::Format::r8g8b8a8Srgb;
		case vk::Format::b8g8r8a8Unorm:
			return vk::Format::b8g8r8a8Srgb;
		case vk::Format::b8g8r8Unorm:
			return vk::Format::b8g8r8Srgb;
		case vk::Format::a8b8g8r8UnormPack32:
			return vk::Format::a8b8g8r8SrgbPack32;

		default: return format;
	}
}

vk::ImageType minImageType(vk::Extent3D size, unsigned minDim) {
	if(size.depth > 1 || minDim > 2) {
		return vk::ImageType::e3d;
	} else if(size.height > 1 || minDim > 1) {
		return vk::ImageType::e2d;
	} else {
		return vk::ImageType::e1d;
	}
}

// NOTE: even if size.y == 1, when cubemap is true, we will return
// cubemap view types (since there are no 1D cube types).
vk::ImageViewType minImageViewType(vk::Extent3D size, unsigned layers,
		bool cubemap, unsigned minDim) {
	if(size.depth > 1 || minDim > 2) {
		dlg_assertm(layers == 0 && cubemap == 0,
			"Layered or cube 3D images are not allowed");
		return vk::ImageViewType::e3d;
	}

	if(cubemap) {
		dlg_assert(layers % 6 == 0u);
		return (layers > 6 ? vk::ImageViewType::cubeArray : vk::ImageViewType::cube);
	}

	if(size.height > 1 || minDim > 1) {
		return layers > 1 ? vk::ImageViewType::e2dArray : vk::ImageViewType::e2d;
	} else {
		return layers > 1 ? vk::ImageViewType::e1dArray : vk::ImageViewType::e1d;
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

Vec4d read(vk::Format srcFormat, span<const std::byte>& src) {
	switch(srcFormat) {
		case vk::Format::r16Sfloat:
			return Vec4d(read<1, f16>(src));
		case vk::Format::r16g16Sfloat:
			return Vec4d(read<2, f16>(src));
		case vk::Format::r16g16b16Sfloat:
			return Vec4d(read<3, f16>(src));
		case vk::Format::r16g16b16a16Sfloat:
			return Vec4d(read<4, f16>(src));

		case vk::Format::r32Sfloat:
			return Vec4d(read<1, float>(src));
		case vk::Format::r32g32Sfloat:
			return Vec4d(read<2, float>(src));
		case vk::Format::r32g32b32Sfloat:
			return Vec4d(read<3, float>(src));
		case vk::Format::r32g32b32a32Sfloat:
			return Vec4d(read<4, float>(src));

		case vk::Format::r64Sfloat:
			return Vec4d(read<1, double>(src));
		case vk::Format::r64g64Sfloat:
			return Vec4d(read<2, double>(src));
		case vk::Format::r64g64b64Sfloat:
			return Vec4d(read<3, double>(src));
		case vk::Format::r64g64b64a64Sfloat:
			return Vec4d(read<4, double>(src));

		case vk::Format::e5b9g9r9UfloatPack32:
			return Vec4d(e5b9g9r9ToRgb(read<u32>(src)));
		default:
			throw std::logic_error("Format not supported for CPU reading");
	}
}

void write(vk::Format dstFormat, span<std::byte>& dst, const Vec4d& color) {
	switch(dstFormat) {
		case vk::Format::r32Sfloat:
			write(dst, float(color[0]));
			break;
		case vk::Format::r32g32Sfloat:
			write(dst, Vec2f(color));
			break;
		case vk::Format::r32g32b32Sfloat:
			write(dst, Vec3f(color));
			break;
		case vk::Format::r32g32b32a32Sfloat:
			write(dst, Vec4f(color));
			break;

		case vk::Format::r64Sfloat:
			write(dst, double(color[0]));
			break;
		case vk::Format::r64g64Sfloat:
			write(dst, Vec2d(color));
			break;
		case vk::Format::r64g64b64Sfloat:
			write(dst, Vec3d(color));
			break;
		case vk::Format::r64g64b64a64Sfloat:
			write(dst, Vec4d(color));
			break;

		case vk::Format::r16Sfloat:
			write(dst, f16(color[0]));
			break;
		case vk::Format::r16g16Sfloat:
			write(dst, Vec2<f16>(color));
			break;
		case vk::Format::r16g16b16Sfloat:
			write(dst, Vec3<f16>(color));
			break;
		case vk::Format::r16g16b16a16Sfloat:
			write(dst, Vec4<f16>(color));
			break;

		case vk::Format::e5b9g9r9UfloatPack32:
			write(dst, e5b9g9r9FromRgb(Vec3f(color)));
			break;

		default:
			throw std::logic_error("Format not supported for CPU writing");
	}
}

void convert(vk::Format dstFormat, span<std::byte>& dst,
		vk::Format srcFormat, span<const std::byte>& src) {
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
