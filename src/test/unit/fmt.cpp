#include <util/util.hpp>
#include <util/fmt.hpp>
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>
#include "../bugged.hpp"
#include "../approx.hpp"

using namespace vil;

namespace vil::bugged {

template<typename T>
struct Printable<Approx<T>> {
	static std::string call(const Approx<T>& approx) {
		static auto txt = std::string("Approx(") + std::to_string(approx.value) + ")";
		return txt;
	}
};

}

TEST(unit_fmt_simple) {
	u8 vals[] = {192, 99, 0, 253};
	auto texelBuf = ReadBuf(bytes(vals));

	// uint
	{
		auto buf = texelBuf;
		auto colors = vil::read(VK_FORMAT_R8G8B8A8_UINT, buf);
		EXPECT(u32(colors[0]), vals[0]);
		EXPECT(u32(colors[1]), vals[1]);
		EXPECT(u32(colors[2]), vals[2]);
		EXPECT(u32(colors[3]), vals[3]);
	}

	// uscaled
	{
		auto buf = texelBuf;
		auto colors = vil::read(VK_FORMAT_R8G8B8A8_USCALED, buf);
		EXPECT(u32(colors[0]), vals[0]);
		EXPECT(u32(colors[1]), vals[1]);
		EXPECT(u32(colors[2]), vals[2]);
		EXPECT(u32(colors[3]), vals[3]);
	}

	// unorm
	{
		auto buf = texelBuf;
		auto colors = vil::read(VK_FORMAT_R8G8B8A8_UNORM, buf);
		EXPECT(colors[0], approx(vals[0] / 255.f));
		EXPECT(colors[1], approx(vals[1] / 255.f));
		EXPECT(colors[2], approx(vals[2] / 255.f));
		EXPECT(colors[3], approx(vals[3] / 255.f));
	}

	// uint but swizzled
	{
		auto buf = texelBuf;
		auto colors = vil::read(VK_FORMAT_B8G8R8A8_UINT, buf);
		EXPECT(u32(colors[0]), vals[2]);
		EXPECT(u32(colors[1]), vals[1]);
		EXPECT(u32(colors[2]), vals[0]);
		EXPECT(u32(colors[3]), vals[3]);
	}
}

TEST(unit_fmt_packed) {
	auto format = VK_FORMAT_A2B10G10R10_UINT_PACK32;

	const auto alpha = 2u;
	const auto blue = 564u;
	const auto red = 933u;
	const auto green = 124u;

	u32 packed = 0u;
	packed = (packed | alpha) << 10u;
	packed = (packed | blue) << 10u;
	packed = (packed | green) << 10u;
	packed = (packed | red);

	// read
	auto texelBuf = ReadBuf(bytes(packed));
	auto colors = vil::read(format, texelBuf);

	EXPECT(u32(colors[0]), red);
	EXPECT(u32(colors[1]), green);
	EXPECT(u32(colors[2]), blue);
	EXPECT(u32(colors[3]), alpha);

	// write
	u32 dst;
	auto dstBuf = bytes(dst);
	vil::write(format, dstBuf, colors);

	const auto packedRed = dst & ((1 << 10) - 1);
	dst >>= 10;
	const auto packedGreen = dst & ((1 << 10) - 1);
	dst >>= 10;
	const auto packedBlue = dst & ((1 << 10) - 1);
	dst >>= 10;
	const auto packedAlpha = dst & ((1 << 2) - 1);

	EXPECT(u32(colors[0]), packedRed);
	EXPECT(u32(colors[1]), packedGreen);
	EXPECT(u32(colors[2]), packedBlue);
	EXPECT(u32(colors[3]), packedAlpha);
}

TEST(unit_fmt_read_write) {
	// the format range we support (with exceptions)
	auto startFormat = VK_FORMAT_R4G4_UNORM_PACK8;
	auto endFormat = VK_FORMAT_BC1_RGB_UNORM_BLOCK;

	// known issues/not implemented
	auto exceptions = std::array{
		VK_FORMAT_B10G11R11_UFLOAT_PACK32
	};

	for(auto fmt = startFormat; fmt != endFormat; fmt = VkFormat(u32(fmt) + 1)) {
		auto it = std::find(exceptions.begin(), exceptions.end(), fmt);
		if(it != exceptions.end()) {
			continue;
		}

		// dlg_trace("fmt_read_write: {}", vk::name(fmt));

		Vec4d vals = {0.2, 0.8, 0.1, 1.0};

		auto normed = FormatIsUNORM(fmt) || FormatIsSNORM(fmt) || FormatIsSRGB(fmt);
		if(!normed) {
			vals[0] = 20.0 * vals[0];
			vals[1] = 30.0 * vals[1];
			vals[2] = 10.0 * vals[2];
			vals[3] = 1; // so we can be sure a2 (even signed) formats work as well
		}

		if(FormatIsDepthAndStencil(fmt)) {
			vals[0] = 0.25; // make sure we have float depth
			vals[1] = 78u; // make sure we have uint stencil
		}

		auto isSigned = FormatIsSINT(fmt) || FormatIsSSCALED(fmt) || FormatIsSNORM(fmt);
		if(isSigned) {
			vals[0] = -vals[0];
		}

		std::byte buf[100];
		auto writeSpan = bytes(buf);
		write(fmt, writeSpan, vals);

		auto readSpan = ReadBuf(bytes(buf));
		auto newVals = read(fmt, readSpan);

		auto count = FormatComponentCount(fmt);
		for(auto i = 0u; i < count; ++i) {
			// need high epsilon here since we compress them quite
			// a lot in some cases
			// TODO: make epsilon dependent on channel size
			auto eps = 0.01;
			if(FormatElementSize(fmt) / count < 8) {
				eps = 0.05;
			}

			EXPECT(newVals[i], approx(vals[i], eps));
		}
	}
}
