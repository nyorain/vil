#include <util/fmt.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/vecOps.hpp>
#include <vk/format_utils.h>
#include <vkutil/enumString.hpp>
#include <cmath>

namespace vil {

using nytl::read;
using nytl::write;

// - https://en.wikipedia.org/wiki/SRGB (conversion matrices from here)
// - https://www.w3.org/Graphics/Color/srgb
double linearToSRGB(double linear) {
	if(linear < 0.0031308) {
		return 12.92 * linear;
	} else {
		return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
	}
}

double srgbToLinear(double srgb) {
	if(srgb < 0.04045) {
		return srgb / 12.92;
	} else {
		return std::pow((srgb + 0.055) / 1.055, 2.4);
	}
}

Vec4d linearToSRGB(Vec4d v) {
	v[0] = linearToSRGB(v[0]);
	v[1] = linearToSRGB(v[1]);
	v[2] = linearToSRGB(v[2]);
	return v;
}

Vec4d srgbToLinear(Vec4d v) {
	v[0] = srgbToLinear(v[0]);
	v[1] = srgbToLinear(v[1]);
	v[2] = srgbToLinear(v[2]);
	return v;
}

// reads formats
struct FormatReader {
	template<std::size_t N, typename T, u32 Fac, bool SRGB>
	static void call(span<const std::byte>& src, Vec4d& dst) {
		using nytl::vec::operators::operator/;
		auto ret = read<Vec<N, T>>(src);
		// TODO: strictly speaking we have to clamp for normed formats
		// (so that we can't ever get 1.0..01 for SNORM formats)
		dst = Vec4d(ret) / double(Fac);
		if constexpr(SRGB) {
			dst = srgbToLinear(dst);
		}
	}

	// packed formats
	template<bool Norm, bool Signed>
	static void unpack(u32& src, Vec4d& dst, u32) {
		// nothing to do, no bits remaining
		(void) src;
		(void) dst;
	}

	template<bool Norm, bool Signed, u32 FirstBits, u32... Rest>
	static void unpack(u32& src, Vec4d& dst, u32 id) {
		unpack<Norm, Signed, Rest...>(src, dst, id + 1);

		auto limit = 1u << FirstBits;
		auto mask = limit - 1; // first (FirstBits-1) bits set to 1

		auto valUint = (src & mask);
		double converted = valUint;

		if constexpr(Signed) {
			auto halfLimit = limit / 2;
			converted = converted - double(halfLimit);
			mask = halfLimit - 1; // if it's normalized
		}

		if constexpr(Norm) {
			converted = converted / double(mask);
		}

		dst[id] = converted;
		src = src >> FirstBits;
	}

	template<bool Norm, bool Signed, bool SRGB, u32... Bits>
	static void callPack(span<const std::byte>& src, Vec4d& dst) {
		u32 packed;
		constexpr u32 numBits = (0 + ... + Bits);
		if constexpr(numBits == 8) {
			packed = read<u8>(src);
		} else if(numBits == 16) {
			packed = read<u16>(src);
		} else {
			static_assert(numBits, "only 8/16/32 bit packed formats supported");
			packed = read<u32>(src);
		}

		unpack<Norm, Signed, Bits...>(packed, dst, 0u);

		// TODO: strictly speaking we have to clamp for normed formats
		// (so that we can't ever get 1.0..01 for SNORM formats)
		using nytl::vec::operators::operator/;

		if constexpr(SRGB) {
			static_assert(!Signed && Norm);
			dst = srgbToLinear(dst);
		}
	}
};

// writes formats
struct FormatWriter {
	template<std::size_t N, typename T, u32 Fac, bool SRGB>
	static void call(span<std::byte>& dst, Vec4d src) {
		if constexpr(SRGB) {
			src = linearToSRGB(src);
		}

		for(auto i = 0u; i < N; ++i) {
			write<T>(dst, T(Fac * src[i]));
		}
	}

	// packed formats
	template<bool Norm, bool Signed>
	static void pack(u32& dst, const Vec4d& src, u32) {
		// nothing to do, no bits remaining
		(void) src;
		(void) dst;
	}

	template<bool Norm, bool Signed, u32 FirstBits, u32... Rest>
	static void pack(u32& dst, const Vec4d& src, u32 id) {
		auto converted = src[id];

		auto limit = 1u << FirstBits;
		auto mask = limit - 1; // first (FirstBits-1) bits set to 1
		[[maybe_unused]] float signFac = 1.0;

		if constexpr(Norm) {
			converted *= double(mask);
			signFac = 0.5; // if it's also signed
		}

		if constexpr(Signed) {
			auto halfLimit = double(limit >> 1u);
			converted = signFac * converted + halfLimit;
		}

		u32 valUint = u32(converted);

		dst = (dst << FirstBits) | (valUint & mask);
		pack<Norm, Signed, Rest...>(dst, src, id + 1);
	}

	template<bool Norm, bool Signed, bool SRGB, u32... Bits>
	static void callPack(span<std::byte>& dst, Vec4d src) {
		if constexpr(SRGB) {
			static_assert(!Signed && Norm);
			src = linearToSRGB(src);
		}

		u32 packed {};
		pack<Norm, Signed, Bits...>(packed, src, 0u);

		constexpr u32 numBits = (0 + ... + Bits);
		if constexpr(numBits == 8) {
			write<u8>(dst, packed);
		} else if(numBits == 16) {
			write<u16>(dst, packed);
		} else {
			static_assert(numBits, "only 8/16/32 bit packed formats supported");
			write<u32>(dst, packed);
		}
	}
};

template<bool Write, std::size_t N, typename T, u32 Fac = 1, bool SRGB = false,
	typename Span, typename Vec>
void iofmt(Span& span, Vec& vec) {
	if constexpr(Write) {
		FormatWriter::call<N, T, Fac, SRGB>(span, vec);
	} else {
		FormatReader::call<N, T, Fac, SRGB>(span, vec);
	}
}

// like iofmt but for packed formats
template<bool Write, bool Norm, bool Signed, bool SRGB, u32... Bits,
	typename Span, typename Vec>
void iopack(Span& span, Vec& vec) {
	if constexpr(Write) {
		FormatWriter::callPack<Norm, Signed, SRGB, Bits...>(span, vec);
	} else {
		FormatReader::callPack<Norm, Signed, SRGB, Bits...>(span, vec);
	}
}

template<bool Write, FORMAT_NUMERICAL_TYPE type, u32... Bits,
	typename Span, typename Vec>
void iopack(Span& span, Vec& vec) {
	using FMT = FORMAT_NUMERICAL_TYPE;
	iopack<Write,
		type == FMT::UNORM || type == FMT::SRGB || type == FMT::SNORM,
		type == FMT::SNORM || type == FMT::SINT || type == FMT::SSCALED,
		type == FMT::SRGB,
		Bits...>(span, vec);
}

// swizzle
template<bool Reverse, unsigned A, unsigned B = 1u, unsigned C = 2u, unsigned D = 3u>
Vec4d swizzle(Vec4d x) {
	static_assert(A <= 3 && B <= 3 && C <= 3 && D <= 3);

	if constexpr(Reverse) {
		Vec4d ret;
		ret[A] = x[0];
		ret[B] = x[1];
		ret[C] = x[2];
		ret[D] = x[3];
		return ret;
	} else {
		return Vec4d{x[A], x[B], x[C], x[D]};
	}
}

template<bool W, typename Span, typename Vec>
void ioFormat(VkFormat format, Span& span, Vec& vec) {
	// TODO: missing:
	// - VK_FORMAT_B10G11R11_UFLOAT_PACK32
	// - (block-)compressed formats (can't be supported with this api anyways i guess)
	// 	- also multiplanar formats. But that's even harder, needs more complex api
	// 	  we'd only want cpu side decoding as a fallback anyways, we usually
	// 	  rely on being able to sample from more complicated formats
	// - Also properly test this!

	using FMT = FORMAT_NUMERICAL_TYPE;

	// We swizzle separately (in the calling function), so rgba here is the
	// same as bgra
	switch(format) {
		case VK_FORMAT_R16_SFLOAT: return iofmt<W, 1, f16>(span, vec);
		case VK_FORMAT_R16G16_SFLOAT: return iofmt<W, 2, f16>(span, vec);
		case VK_FORMAT_R16G16B16_SFLOAT: return iofmt<W, 3, f16>(span, vec);
		case VK_FORMAT_R16G16B16A16_SFLOAT: return iofmt<W, 4, f16>(span, vec);

		case VK_FORMAT_R32_SFLOAT: return iofmt<W, 1, float>(span, vec);
		case VK_FORMAT_R32G32_SFLOAT: return iofmt<W, 2, float>(span, vec);
		case VK_FORMAT_R32G32B32_SFLOAT: return iofmt<W, 3, float>(span, vec);
		case VK_FORMAT_R32G32B32A32_SFLOAT: return iofmt<W, 4, float>(span, vec);

		case VK_FORMAT_R64_SFLOAT: return iofmt<W, 1, double>(span, vec);
		case VK_FORMAT_R64G64_SFLOAT: return iofmt<W, 2, double>(span, vec);
		case VK_FORMAT_R64G64B64_SFLOAT: return iofmt<W, 3, double>(span, vec);
		case VK_FORMAT_R64G64B64A64_SFLOAT: return iofmt<W, 4, double>(span, vec);

		case VK_FORMAT_R8_UNORM: return iofmt<W, 1, u8, 255>(span, vec);
		case VK_FORMAT_R8G8_UNORM: return iofmt<W, 2, u8, 255>(span, vec);
		case VK_FORMAT_B8G8R8_UNORM:
		case VK_FORMAT_R8G8B8_UNORM:
			return iofmt<W, 3, u8, 255>(span, vec);
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_UNORM:
			return iofmt<W, 4, u8, 255>(span, vec);

		case VK_FORMAT_R8_SRGB: return iofmt<W, 1, u8, 255, true>(span, vec);
		case VK_FORMAT_R8G8_SRGB: return iofmt<W, 2, u8, 255, true>(span, vec);
		case VK_FORMAT_B8G8R8_SRGB:
		case VK_FORMAT_R8G8B8_SRGB:
			return iofmt<W, 3, u8, 255, true>(span, vec);
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_SRGB:
			return iofmt<W, 4, u8, 255, true>(span, vec);

		case VK_FORMAT_R16_UNORM: return iofmt<W, 1, u16, 65535>(span, vec);
		case VK_FORMAT_R16G16_UNORM: return iofmt<W, 2, u16, 65535>(span, vec);
		case VK_FORMAT_R16G16B16_UNORM: return iofmt<W, 3, u16, 65535>(span, vec);
		case VK_FORMAT_R16G16B16A16_UNORM: return iofmt<W, 4, u16, 65535>(span, vec);

		case VK_FORMAT_R8_SNORM: return iofmt<W, 1, i8, 127>(span, vec);
		case VK_FORMAT_R8G8_SNORM: return iofmt<W, 2, i8, 127>(span, vec);
		case VK_FORMAT_B8G8R8_SNORM:
		case VK_FORMAT_R8G8B8_SNORM:
			return iofmt<W, 3, i8, 127>(span, vec);
		case VK_FORMAT_R8G8B8A8_SNORM:
		case VK_FORMAT_B8G8R8A8_SNORM:
			return iofmt<W, 4, i8, 127>(span, vec);

		case VK_FORMAT_R16_SNORM: return iofmt<W, 1, i16, 32767>(span, vec);
		case VK_FORMAT_R16G16_SNORM: return iofmt<W, 2, i16, 32767>(span, vec);
		case VK_FORMAT_R16G16B16_SNORM: return iofmt<W, 3, i16, 32767>(span, vec);
		case VK_FORMAT_R16G16B16A16_SNORM: return iofmt<W, 4, i16, 32767>(span, vec);

		case VK_FORMAT_R8_USCALED:
		case VK_FORMAT_R8_UINT: return iofmt<W, 1, u8>(span, vec);
		case VK_FORMAT_R8G8_USCALED:
		case VK_FORMAT_R8G8_UINT: return iofmt<W, 2, u8>(span, vec);
		case VK_FORMAT_R8G8B8_USCALED:
		case VK_FORMAT_R8G8B8_UINT:
		case VK_FORMAT_B8G8R8_USCALED:
		case VK_FORMAT_B8G8R8_UINT:
			return iofmt<W, 3, u8>(span, vec);
		case VK_FORMAT_R8G8B8A8_USCALED:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_B8G8R8A8_USCALED:
		case VK_FORMAT_B8G8R8A8_UINT:
			return iofmt<W, 4, u8>(span, vec);

		case VK_FORMAT_R16_USCALED:
		case VK_FORMAT_R16_UINT: return iofmt<W, 1, u16>(span, vec);
		case VK_FORMAT_R16G16_USCALED:
		case VK_FORMAT_R16G16_UINT: return iofmt<W, 2, u16>(span, vec);
		case VK_FORMAT_R16G16B16_USCALED:
		case VK_FORMAT_R16G16B16_UINT: return iofmt<W, 3, u16>(span, vec);
		case VK_FORMAT_R16G16B16A16_USCALED:
		case VK_FORMAT_R16G16B16A16_UINT: return iofmt<W, 4, u16>(span, vec);

		case VK_FORMAT_R32_UINT: return iofmt<W, 1, u32>(span, vec);
		case VK_FORMAT_R32G32_UINT: return iofmt<W, 2, u32>(span, vec);
		case VK_FORMAT_R32G32B32_UINT: return iofmt<W, 3, u32>(span, vec);
		case VK_FORMAT_R32G32B32A32_UINT: return iofmt<W, 4, u32>(span, vec);

		case VK_FORMAT_R8_SSCALED:
		case VK_FORMAT_R8_SINT: return iofmt<W, 1, i8>(span, vec);
		case VK_FORMAT_R8G8_SSCALED:
		case VK_FORMAT_R8G8_SINT: return iofmt<W, 2, i8>(span, vec);
		case VK_FORMAT_R8G8B8_SSCALED:
		case VK_FORMAT_R8G8B8_SINT:
		case VK_FORMAT_B8G8R8_SSCALED:
		case VK_FORMAT_B8G8R8_SINT:
			return iofmt<W, 3, i8>(span, vec);
		case VK_FORMAT_R8G8B8A8_SSCALED:
		case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_B8G8R8A8_SSCALED:
		case VK_FORMAT_B8G8R8A8_SINT:
			return iofmt<W, 4, i8>(span, vec);

		case VK_FORMAT_R16_SSCALED:
		case VK_FORMAT_R16_SINT: return iofmt<W, 1, i16>(span, vec);
		case VK_FORMAT_R16G16_SSCALED:
		case VK_FORMAT_R16G16_SINT: return iofmt<W, 2, i16>(span, vec);
		case VK_FORMAT_R16G16B16_SSCALED:
		case VK_FORMAT_R16G16B16_SINT: return iofmt<W, 3, i16>(span, vec);
		case VK_FORMAT_R16G16B16A16_SSCALED:
		case VK_FORMAT_R16G16B16A16_SINT: return iofmt<W, 4, i16>(span, vec);

		case VK_FORMAT_R32_SINT: return iofmt<W, 1, i32>(span, vec);
		case VK_FORMAT_R32G32_SINT: return iofmt<W, 2, i32>(span, vec);
		case VK_FORMAT_R32G32B32_SINT: return iofmt<W, 3, i32>(span, vec);
		case VK_FORMAT_R32G32B32A32_SINT: return iofmt<W, 4, i32>(span, vec);

		// NOTE: precision for 64-bit int formats can be problematic
		case VK_FORMAT_R64_UINT: return iofmt<W, 1, u64>(span, vec);
		case VK_FORMAT_R64G64_UINT: return iofmt<W, 2, u64>(span, vec);
		case VK_FORMAT_R64G64B64_UINT: return iofmt<W, 3, u64>(span, vec);
		case VK_FORMAT_R64G64B64A64_UINT: return iofmt<W, 4, u64>(span, vec);

		// NOTE: precision for 64-bit int formats can be problematic
		case VK_FORMAT_R64_SINT: return iofmt<W, 1, i64>(span, vec);
		case VK_FORMAT_R64G64_SINT: return iofmt<W, 2, i64>(span, vec);
		case VK_FORMAT_R64G64B64_SINT: return iofmt<W, 3, i64>(span, vec);
		case VK_FORMAT_R64G64B64A64_SINT: return iofmt<W, 4, i64>(span, vec);

		// packed
		case VK_FORMAT_R4G4_UNORM_PACK8: return iopack<W, FMT::UNORM, 4, 4>(span, vec);
		case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
		case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
		case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
			return iopack<W, FMT::UNORM, 5, 5, 5, 1>(span, vec);
		case VK_FORMAT_B5G6R5_UNORM_PACK16:
		case VK_FORMAT_R5G6B5_UNORM_PACK16:
			return iopack<W, FMT::UNORM, 5, 6, 5>(span, vec);
		case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
		case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
			return iopack<W, FMT::UNORM, 4, 4, 4, 4>(span, vec);

		case VK_FORMAT_A8B8G8R8_SINT_PACK32: return iopack<W, FMT::SINT, 8, 8, 8, 8>(span, vec);
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return iopack<W, FMT::SRGB, 8, 8, 8, 8>(span, vec);
		case VK_FORMAT_A8B8G8R8_UINT_PACK32: return iopack<W, FMT::UINT, 8, 8, 8, 8>(span, vec);
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return iopack<W, FMT::UNORM, 8, 8, 8, 8>(span, vec);
		case VK_FORMAT_A8B8G8R8_SNORM_PACK32: return iopack<W, FMT::SNORM, 8, 8, 8, 8>(span, vec);
		case VK_FORMAT_A8B8G8R8_SSCALED_PACK32: return iopack<W, FMT::SSCALED, 8, 8, 8, 8>(span, vec);
		case VK_FORMAT_A8B8G8R8_USCALED_PACK32: return iopack<W, FMT::USCALED, 8, 8, 8, 8>(span, vec);

		case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
			return iopack<W, FMT::SNORM, 2, 10, 10, 10>(span, vec);

		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
			return iopack<W, FMT::UNORM, 2, 10, 10, 10>(span, vec);

		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
			return iopack<W, FMT::UINT, 2, 10, 10, 10>(span, vec);

		case VK_FORMAT_A2B10G10R10_SINT_PACK32:
		case VK_FORMAT_A2R10G10B10_SINT_PACK32:
			return iopack<W, FMT::SINT, 2, 10, 10, 10>(span, vec);

		case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
			return iopack<W, FMT::USCALED, 2, 10, 10, 10>(span, vec);

		case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
			return iopack<W, FMT::SSCALED, 2, 10, 10, 10>(span, vec);

		// depth-stencil formats.
		case VK_FORMAT_S8_UINT: return iofmt<W, 1, u8>(span, vec);
		case VK_FORMAT_D16_UNORM: return iofmt<W, 1, u16, 65535>(span, vec);
		case VK_FORMAT_D16_UNORM_S8_UINT:
			if constexpr(W) {
				write(span, u16(vec[0] * 65535));
				write(span, u8(vec[1]));
			} else {
				vec = {};
				vec[0] = read<u16>(span) / 65535.0;
				vec[1] = read<u8>(span);
			}
			break;
		case VK_FORMAT_D24_UNORM_S8_UINT:
			if constexpr(W) {
				u32 d = 16777215 * vec[0];
				write(span, u8((d >> 16) & 0xFF));
				write(span, u8((d >> 8) & 0xFF));
				write(span, u8((d) & 0xFF));
				write(span, u8(vec[1]));
			} else {
				vec = {};
				auto d = read<std::array<u8, 3>>(span);
				vec[0] = ((u32(d[0]) << 16) | (u32(d[1]) << 8) | u32(d[2])) / 16777215.0;
				vec[1] = read<u8>(span);
			}
			break;
		case VK_FORMAT_X8_D24_UNORM_PACK32:
			if constexpr(W) {
				auto fac = (1u << 24) - 1;
				u32 d = u32(fac * vec[0]);
				write(span, d);
			} else {
				vec = {};
				auto d = read<u32>(span);
				auto mask = (1u << 24) - 1;
				vec[0] = (d & mask) / double(mask);
			}
			break;
		case VK_FORMAT_D32_SFLOAT: return iofmt<W, 1, float>(span, vec);
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			if constexpr(W) {
				write(span, float(vec[0]));
				write(span, u8(vec[1]));
			} else {
				vec = {};
				vec[0] = read<float>(span);
				vec[1] = read<u8>(span);
			}
			break;

		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			if constexpr(W) {
				write(span, e5b9g9r9FromRgb(Vec3f(vec)));
			} else {
				vec = Vec4d(e5b9g9r9ToRgb(read<u32>(span)));
			}
			break;

		default:
			dlg_error("Format '{}' not supported for CPU reading/writing", vk::name(format));
			break;
	}
}

template<bool W>
Vec4d formatSwizzle(VkFormat format, Vec4d x) {
	switch(format) {
		// bgra -> rgba
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_SINT:
		case VK_FORMAT_B8G8R8A8_UINT:
		case VK_FORMAT_B8G8R8A8_SSCALED:
		case VK_FORMAT_B8G8R8A8_USCALED:
		case VK_FORMAT_B8G8R8_UNORM:
		case VK_FORMAT_B8G8R8_SRGB:
		case VK_FORMAT_B8G8R8_SINT:
		case VK_FORMAT_B8G8R8_UINT:
		case VK_FORMAT_B8G8R8_SSCALED:
		case VK_FORMAT_B8G8R8_USCALED:
		// packed
		case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
		case VK_FORMAT_B5G6R5_UNORM_PACK16:
		case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
			return swizzle<W, 2, 1, 0, 3>(x);
		// (packed) abgr -> rgba
		case VK_FORMAT_A8B8G8R8_SINT_PACK32:
		case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		case VK_FORMAT_A8B8G8R8_UINT_PACK32:
		case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
		case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
		case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
		case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_A2B10G10R10_SINT_PACK32:
		case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
		case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
			return swizzle<W, 3, 2, 1, 0>(x);
		// (packed) argb -> rgba
		case VK_FORMAT_A2R10G10B10_SINT_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
			return swizzle<W, 1, 2, 3, 0>(x);
		default:
			return x;
	}
}

Vec4d read(VkFormat srcFormat, span<const std::byte>& src) {
	Vec4d ret {};
	ioFormat<false>(srcFormat, src, ret);
	ret = formatSwizzle<false>(srcFormat, ret);
	return ret;
}

void write(VkFormat dstFormat, span<std::byte>& dst, const Vec4d& color) {
	const auto sc = formatSwizzle<true>(dstFormat, color);
	ioFormat<true>(dstFormat, dst, sc);
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

} // namespace vil
