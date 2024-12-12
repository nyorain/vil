#pragma once

#include <fwd.hpp>
#include <nytl/vec.hpp>
#include <util/intrusive.hpp>
#include <imgui/textedit.h>
#include <util/patch.hpp>
#include <spvm/program.h>
#include <spvm/state.h>
#include <spvm/image.h>
#include <spvm/result.h>
#include <nytl/bytes.hpp>
#include <vk/vulkan.h>
#include <deque>
#include <unordered_map>
#include <vector>
#include <future>

namespace vil {

// from buffmt
struct Type;
class ShaderDebugger;
struct ShaderEmulation;

struct ShaderDebugPatch {
	u32 file {u32(-1)};
	u32 line {u32(-1)};

	struct PendingJob {
		std::future<PatchJobResult> result;
		std::unique_ptr<PatchJobData> data;
	};

	std::vector<PendingJob> jobs;
	PatchJobResult current;

	// TODO: find proper solution
	std::vector<vku::Pipeline> keepAlive;

	void updateJobs(Device& dev);
	void reset(Device& dev);
};

class ShaderDebugger {
public:
	ShaderDebugger();
	~ShaderDebugger();
	void init(Gui& gui);

	// Select takes its own copy of a spc::Compiler mainly
	// because of the specialization constant problematic
	void select(VkShaderStageFlagBits stage,
		std::unique_ptr<spc::Compiler> compiled,
		std::string entryPoint);
	void unselect();
	void draw();

	// compute stuff
	Vec3ui workgroupSize() const;
	Vec3ui numWorkgroups() const;
	const auto& breakpoints() const { return breakpoints_; }
	const auto& compiled() const { return compiled_; }
	const auto& globalInvocationID() const { return globalInvocationID_; }
	CommandSelection& selection() const;
	ShaderEmulation* emulation() const { return emu_.get(); }
	void updateHooks(CommandHook&);

private:
	// vertex stuff
	struct DrawCmdInfo {
		i32 vertexOffset; // for non-indexed this is 'firstVertex'
		u32 numVerts; // for indexed, this is 'instanceCount'
		u32 numInis;
		u32 firstIni;
		u32 firstIndex {}; // only for indexed draw
	};

	struct DrawInfo {
		u32 numCommands {};
		bool indexed {};
	};

	DrawInfo drawInfo() const;
	DrawCmdInfo drawCmdInfo(u32 cmd = 0u) const;

	const std::string& fileName(u32 fileID) const;
	const std::string& fileContent(u32 fileID) const;
	void drawInputsTab();

	std::string_view fileContents(u32 fileID);

	// toggles breakpoint on current line
	void toggleBreakpoint();

private:
	struct Location {
		u32 fileID;
		u32 lineID;
		const int* pos;
	};

	friend inline bool operator==(const Location& a, const Location& b) {
		return a.fileID == b.fileID && a.lineID == b.lineID;
	}

	// Only for comparison
	CommandHookState* lastHookState_ {};

	Gui* gui_ {};
	igt::TextEditor textedit_;
	std::unique_ptr<spc::Compiler> compiled_ {};
	std::string entryPoint_ {};
	VkShaderStageFlagBits stage_ {};

	std::vector<Location> breakpoints_;
	std::vector<u32> sourceFilesIDs_;
	u32 currentFile_ {};

	Vec3ui globalInvocationID_ {0u, 0u, 0u};
	u32 commandID_ {0u};
	u32 instanceID_ {0u};
	u32 vertexID_ {0u}; // might also be instance id

	std::unique_ptr<ShaderEmulation> emu_ {};

	bool livePatch_ {true};
	ShaderDebugPatch patch_ {};
};

struct ShaderEmulation {
public:
	ShaderEmulation(ShaderDebugger& dbg) : dbg_(&dbg) {}
	~ShaderEmulation();

	void init();
	void select();
	void unselect();

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
	void stepLine();

	// Sets the text editor to the current line of the state.
	void updatePosition(bool moveCursor = true);

	void drawInputsTab();
	void drawInputsCompute();
	void drawInputsVertex();
	void drawVariablesTab();
	void drawBreakpointsTab();
	void drawCallstackTab();
	void drawControlTabs();

	void updateHooks(CommandHook&);
	void initVarMap();
	ShaderDebugger& shaderDebugger() { return *dbg_; }

	// imgui
	void drawControls();
	void updateFromEditor();

private:
	struct {
		spvm_context_t context {};
		spvm_program_t program {};
		spvm_state_t state {};
	} spvm_;

	struct OurImage : spvm_image {
		ReadBuf data;
		VkFormat format; // of data
	};

	static const OurImage emptyImage;
	static const spvm_sampler defaultSampler;

	std::deque<spvm_sampler> samplers_;
	std::deque<OurImage> images_;

	std::unordered_map<u32, u32> varIDToDsCopyMap_;
	ShaderDebugger* dbg_;

	bool rerun_ {};
	bool freezeOnBreakPoint_ {};

	u32 currLine_ {};
	std::string currFileName_ {};

};

} // namespace vil

