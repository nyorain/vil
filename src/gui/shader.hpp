#pragma once

#include <fwd.hpp>
#include <imgui/textedit.h>
#include <spvm/program.h>
#include <spvm/state.h>
#include <spvm/image.h>
#include <spvm/result.h>
#include <util/bytes.hpp>
#include <deque>

namespace vil {

// from buffmt
struct Type;

class ShaderDebugger {
public:
	~ShaderDebugger();
	void init(Gui& gui);

	// TODO: figure out ownership of spc::Compiler. Kinda difficult due
	// to setting of spec constants. Just copy it?
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

	spvm_value_type valueType(const spvm_member& member);

	// formatting spvm_result/spvm_member for debug table
	void display(const char* name, const spvm_member& members);
	std::string formatScalar(const spvm_member& member);

private:
	struct OurImage : spvm_image {
		ReadBuf data;
	};

	struct Location {
		u32 fileID;
		u32 lineID;
	};

	std::deque<spvm_sampler> samplers_;
	std::deque<OurImage> images_;

	std::unordered_map<std::string_view, spvm_result*> vars_;

	bool refresh_ {};
	u32 currLine_ {};
	std::string currFileName_ {};

	Gui* gui_ {};
	igt::TextEditor textedit_;
	const spc::Compiler* compiled_ {};

	spvm_context_t context_ {};
	spvm_program_t program_ {};
	spvm_state_t state_ {};

	std::unordered_map<u32, u32> varIDToDsCopyMap_;
	std::vector<Location> breakpoints_;
};

} // namespace vil

