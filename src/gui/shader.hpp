#pragma once

#include <fwd.hpp>
#include <imgui/textedit.h>
#include <spvm/program.h>
#include <spvm/state.h>
#include <spvm/image.h>
#include <spvm/result.h>

namespace vil {

// from buffmt
struct Type;

struct ShaderDebugger {
	Gui* gui {};
	igt::TextEditor textedit;
	const spc::Compiler* compiled;

	spvm_context_t context {};
	spvm_program_t program {};
	spvm_state_t state {};

	std::unordered_map<u32, u32> varIDToDsCopyMap;

	~ShaderDebugger();
	void init(Gui& gui);

	// TODO: figure out ownership of spc::Compiler. Kinda difficult due
	// to setting of spec constants
	void select(const spc::Compiler& compiled);
	void draw();

	spvm_member_list loadVar(unsigned id, span<const spvm_word> indices);
	void storeVar(unsigned id, span<const spvm_word> indices,
		spvm_member_list);

	spvm_vec4f readImage(spvm_image&, int x, int y, int z, int layer, int level);
	void writeImage(spvm_image&, int x, int y, int z, int layer, int level,
		const spvm_vec4f&);

	void updateHooks(CommandHook&);

	spvm_member setupMembers(const Type& type, ReadBuf);
	spvm_member setupMembersArray(span<const u32> arrayDims, const Type& type, ReadBuf);
	spvm_member setupVector(const Type& type, u32 stride, ReadBuf);
	spvm_member setupScalar(const Type&, ReadBuf);

	static spvm_sampler_desc setupSampler(const Sampler& src);

	std::vector<std::vector<spvm_member>> members_;

	struct OurSampler : spvm_sampler {
		spvm_sampler_desc desc;
	};

	struct OurImage : spvm_image {
		ReadBuf data;
	};

	std::deque<OurSampler> samplers_;
	std::deque<OurImage> images_;
};

} // namespace vil

