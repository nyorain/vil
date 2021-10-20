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
	void unselect();
	void draw();

	void updateHooks(CommandHook&);

private:
	void loadBuiltin(const spc::BuiltInResource& builtin,
		span<const spvm_word> indices, span<spvm_member> dst);
	void loadVar(unsigned srcID, span<const spvm_word> indices,
		span<spvm_member> dst, u32 typeID);
	void storeVar(unsigned dstID, span<const spvm_word> indices,
		span<spvm_member> src, u32 typeID);

	spvm_vec4f readImage(spvm_image&, int x, int y, int z, int layer, int level);
	void writeImage(spvm_image&, int x, int y, int z, int layer, int level,
		const spvm_vec4f&);

	void setupMember(const Type& type, ReadBuf, spvm_member& dst);
	void setupMemberArray(span<const u32> arrayDims, const Type& type, ReadBuf, spvm_member& dst);
	void setupVector(const Type& type, u32 stride, ReadBuf, spvm_member& dst);
	void setupScalar(const Type&, ReadBuf, spvm_member& dst);

	void initState();

	static spvm_sampler_desc setupSampler(const Sampler& src);

	spvm_value_type valueType(spvm_member& member);

private:
	struct OurImage : spvm_image {
		ReadBuf data;
	};

	std::deque<spvm_sampler> samplers_;
	std::deque<OurImage> images_;

	std::unordered_map<std::string_view, spvm_result*> vars_;
};

} // namespace vil

