#pragma once

#include <fwd.hpp>
#include <nytl/vec.hpp>
#include <util/intrusive.hpp>
#include <imgui/textedit.h>
#include <util/patch.hpp>
#include <nytl/bytes.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <future>

namespace vil {

// from buffmt
struct Type;
class ShaderDebugger;

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

	bool updateJobs(Device& dev);
	void reset(Device& dev);
};

class ShaderDebugger {
public:
	ShaderDebugger();
	~ShaderDebugger();
	void init(Gui& gui);

	void select(const Pipeline& pipe);
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
	const auto& invocationID() const { return invocationID_; }
	CommandSelection& selection() const;
	void updateHooks(CommandHook&);

private:
	// vertex stuff
	struct DrawCmdInfo {
		i32 vertexOffset; // for non-indexed this is 'firstVertex'
		u32 numVerts; // for indexed, this is 'indexCount'
		u32 numInis;
		u32 firstIni;
		u32 firstIndex {}; // only for indexed draw
	};

	struct DrawInfo {
		u32 numCommands {};
		bool indexed {};
	};

	enum class FragmentMode {
		none,
		cursor,
		cursorClicked,
		count,
	};

	const char* name(FragmentMode);

	DrawInfo drawInfo() const;
	DrawCmdInfo drawCmdInfo(u32 cmd = 0u) const;

	const std::string& fileName(u32 fileID) const;
	const std::string& fileContent(u32 fileID) const;
	void drawInputsTab();
	void drawInputsCompute();
	void drawInputsVertex();
	void drawInputsFragment();

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
	Vec3ui invocationID_ {0u, 0u, 0u};
	FragmentMode fragmentMode_ {};

	bool livePatch_ {true};
	ShaderDebugPatch patch_ {};
};

} // namespace vil

