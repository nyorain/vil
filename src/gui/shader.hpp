#pragma once

#include <fwd.hpp>
#include <nytl/vec.hpp>
#include <util/intrusive.hpp>
#include <imgui/textedit.h>
#include <spvm/program.h>
#include <spvm/state.h>
#include <spvm/image.h>
#include <spvm/result.h>
#include <nytl/bytes.hpp>
#include <vk/vulkan.h>
#include <deque>
#include <unordered_map>
#include <vector>

namespace vil {

// from buffmt
struct Type;

class ShaderDebugger {
public:
	ShaderDebugger();
	~ShaderDebugger();
	void init(Gui& gui);

	// Select takes its own copy of a spc::Compiler mainly
	// because of the specialization constant problematic
	void select(std::unique_ptr<spc::Compiler> compiled);
	void unselect();
	void draw();

	void updateHooks(CommandHook&);
	void initVarMap();

private:
	void loadVar(unsigned srcID, span<const spvm_word> indices,
		span<spvm_member> dst, u32 typeID);
	void loadBuiltin(const spc::BuiltInResource& builtin,
		span<const spvm_word> indices, span<spvm_member> dst);
	void storeVar(unsigned dstID, span<const spvm_word> indices,
		span<spvm_member> src, u32 typeID);

	// Returns (type, offset) tuple for accessing the sub-type
	// given by the given indices (as usually defined by SPIR-V) in
	// the given typeID.
	// Requires the total dataSize of the original type/buffer to
	// correctly handle runtime arrays.
	std::pair<const Type*, u32> accessBuffer(ThreadMemScope& tms,
		unsigned typeID, span<const spvm_word> indices, u32 dataSize);

	spvm_vec4f readImage(spvm_image&, int x, int y, int z, int layer, int level);
	void writeImage(spvm_image&, int x, int y, int z, int layer, int level,
		const spvm_vec4f&);
	unsigned arrayLength(unsigned varID, span<const spvm_word> indices);

	void setupMember(const Type& type, ReadBuf, spvm_member& dst);
	void setupMemberArray(span<const u32> arrayDims, const Type& type, ReadBuf, spvm_member& dst);
	void setupVector(const Type& type, u32 stride, ReadBuf, spvm_member& dst);
	void setupScalar(const Type&, ReadBuf, spvm_member& dst);

	// (Re-)Initialized the spvm state.
	void initState();

	// Converts the information of the given sampler to a spvm_sampler_desc.
	static spvm_sampler_desc setupSampler(const Sampler& src);

	spvm_value_type valueType(const spvm_member& member);

	// formatting spvm_result/spvm_member for debug table
	void display(const char* name, const spvm_member& members);
	std::string formatScalar(const spvm_member& member);

	// executes a single opcode. Returns true if a breakpoint was hit.
	bool stepOpcode();

	// Sets the text editor to the current line of the state.
	void updatePosition(bool moveCursor = true);

	void drawInputsTab();
	void drawVariablesTab();
	void drawBreakpointsTab();
	void drawCallstackTab();

	// NOTE: the returned string view is only valid until the state
	// gets recreated (which might happen every frame).
	std::string_view fileName(u32 fileID) const;
	u32 fileID(std::string_view fileName) const; // returns u32(-1) if not found

	// toggles breakpoint on current line
	void toggleBreakpoint();

	void stepLine();

	// compute stuff
	Vec3ui workgroupSize() const;
	Vec3ui numWorkgroups() const;

	CommandSelection& selection() const;

private:
	struct OurImage : spvm_image {
		ReadBuf data;
		VkFormat format; // of data
	};

	static const OurImage emptyImage;
	static const spvm_sampler defaultSampler;

	struct Location {
		u32 fileID;
		u32 lineID;
		const int* pos;
	};

	friend inline bool operator==(const Location& a, const Location& b) {
		return a.fileID == b.fileID && a.lineID == b.lineID;
	}

	std::deque<spvm_sampler> samplers_;
	std::deque<OurImage> images_;

	bool rerun_ {};
	bool freezeOnBreakPoint_ {};
	u32 currLine_ {};
	std::string currFileName_ {};

	// Only for comparison
	CommandHookState* lastHookState_ {};

	Gui* gui_ {};
	igt::TextEditor textedit_;
	std::unique_ptr<spc::Compiler> compiled_ {};

	std::unordered_map<u32, u32> varIDToDsCopyMap_;
	std::vector<Location> breakpoints_;

	Vec3ui globalInvocationID_ {0u, 0u, 0u};
	bool allowSelectOutOfBounds_ {};

	struct {
		spvm_context_t context {};
		spvm_program_t program {};
		spvm_state_t state {};
	} spvm_;
};

} // namespace vil

