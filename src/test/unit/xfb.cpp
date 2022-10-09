#include <shader.hpp>
#include <util/spirv.hpp>
#include <util/dlg.hpp>
#include <nytl/span.hpp>
#include <nytl/bytes.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include "../bugged.hpp"
#include "../data/a.vert.spv.h" // see a.vert; compiled manually

using namespace vil;

const XfbCapture& getCapture(const XfbPatchDesc& desc, const char* name) {
	for(auto& d : desc.captures) {
		if(d.name == name) {
			return d;
		}
	}

	dlg_error("Unreachable: couldn't find capture {}", name);
	throw std::runtime_error("Could not find capture");
}

const XfbCapture& getCapture(const XfbPatchDesc& desc, spv11::BuiltIn builtin) {
	for(auto& d : desc.captures) {
		if(d.builtin && spv11::BuiltIn(*d.builtin) == builtin) {
			return d;
		}
	}

	dlg_error("Unreachable: couldn't find capture builtin {}", u32(builtin));
	throw std::runtime_error("Could not find capture");
}

TEST(unit_xfb_patch) {
	spc::Compiler compiled{{std::begin(a_vert_spv_data), std::end(a_vert_spv_data)}};
	compiled.set_entry_point("main", spv::ExecutionModelVertex);

	auto patched = patchSpirvXfb(compiled, "main");
	EXPECT(patched.desc.get() != nullptr, true);
	EXPECT(patched.desc->captures.size(), 5u);
	EXPECT(patched.desc->stride, u32(16 + 4 + 16 + 1 * 16 + 16));

	auto& pos = getCapture(*patched.desc, spv11::BuiltIn::Position);
	EXPECT(pos.array.size(), 0u);
	EXPECT(pos.vecsize, 4u);
	EXPECT(pos.columns, 1u);
	EXPECT(pos.width, 32u);
	EXPECT(pos.type, XfbCapture::typeFloat);

	auto& out4 = getCapture(*patched.desc, "outStruct.out4");
	EXPECT(out4.array.size(), 1u);
	EXPECT(out4.array[0], 1u);
	EXPECT(out4.vecsize, 4u);
	EXPECT(out4.width, 32u);
	EXPECT(out4.type, XfbCapture::typeFloat);
	EXPECT(out4.builtin, std::nullopt);

	auto& out2 = getCapture(*patched.desc, "out2");
	EXPECT(out2.array.size(), 0u);
	EXPECT(out2.vecsize, 1u);
	EXPECT(out2.width, 32u);
	EXPECT(out2.type, XfbCapture::typeUint);
	EXPECT(out2.builtin, std::nullopt);
}

TEST(unit_xfb_patch_spec) {
	spc::Compiler compiled{{std::begin(a_vert_spv_data), std::end(a_vert_spv_data)}};
	compiled.set_entry_point("main", spv::ExecutionModelVertex);

	// set specialization constant to '8u'
	auto specConstants = compiled.get_specialization_constants();
	EXPECT(specConstants.size(), 1u);
	EXPECT(specConstants[0].constant_id, 0u);
	auto& constant = compiled.get_constant(specConstants[0].id);
	constant.m.c[0].r[0].u32 = 8u;

	auto patched = patchSpirvXfb(compiled, "main");
	EXPECT(patched.desc.get() != nullptr, true);
	EXPECT(patched.desc->captures.size(), 5u);
	EXPECT(patched.desc->stride, u32(16 + 4 + 16 + 3 * 16 + 16));

	auto& out4 = getCapture(*patched.desc, "outStruct.out4");
	EXPECT(out4.array.size(), 1u);
	EXPECT(out4.array[0], 3u);
	EXPECT(out4.vecsize, 4u);
	EXPECT(out4.width, 32u);
	EXPECT(out4.type, XfbCapture::typeFloat);
	EXPECT(out4.builtin, std::nullopt);
}
