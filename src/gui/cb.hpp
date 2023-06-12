#pragma once

#include <fwd.hpp>
#include <command/record.hpp>
#include <gui/render.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/command.hpp>
#include <gui/update.hpp>
#include <gui/commandSelection.hpp>
#include <nytl/flags.hpp>

namespace vil {

struct FrameSubmission;
struct FrameMatch;
struct StateSaver;
struct StateLoader;

class CommandRecordGui {
public:
	CommandRecordGui() = default;
	~CommandRecordGui();

	void init(Gui& gui);
	void draw(Draw& draw);

	// swapchain only guaranteed to stay valid during call
	// TODO: somewhat misleading, will not consider the given swapchain but just
	// use dev.swapchain for updates. See todo on multi-swapchain support
	void showSwapchainSubmissions(Swapchain& swapchain, bool initial = false);
	void select(IntrusivePtr<CommandRecord> record, Command* cmd = nullptr);
	void select(IntrusivePtr<CommandRecord> record, CommandBufferPtr cb);
	void showLocalCaptures(LocalCapture& lc);

	auto& commandViewer() { return commandViewer_; }
	auto& selector() { return selector_; }

private:
	void updateFromSelector();

	void displaySparseBind(FrameSubmission& batch, u32 subID);
	void displaySubmission(FrameSubmission& batch, u32 subID);
	void displayBatch(FrameSubmission&, u32 batchID);
	void displayFrameCommands(Swapchain&);
	void displayRecordCommands();
	void clearSelection(bool unselectCommandViewer);
	void drawSelected(Draw& draw);

	// In single-record mode, updates the shown record.
	// When the given newCommand is empty, will try to find it in the record.
	void updateRecord(IntrusivePtr<CommandRecord> record,
		std::vector<const Command*> newCommand);

	// In frame-record mode, updates the shown frame.
	// When the given newRecord is empty, will try to find it in the frame.
	// When the given newCommand is empty, will try to find it in the record.
	void updateRecords(const FrameMatch&, std::vector<FrameSubmission>&&,
		IntrusivePtr<CommandRecord> newRecord,
		std::vector<const Command*> newCommand);

	// Helper for the function above, will perform a full frame match.
	void updateRecords(std::vector<FrameSubmission>,
		IntrusivePtr<CommandRecord> newRecord,
		std::vector<const Command*> newCommand);

	void showLoadPopup();
	void showSavePopup();
	void saveSelection(std::string_view name);
	void loadSelection(std::string_view name);
	void save(StateSaver&, DynWriteBuf& buf);
	void load(StateLoader&, LoadBuf& buf);
	void loadStartup();

private:
	friend class Gui;
	Gui* gui_ {};
	bool freezeCommands_ {};
	bool actionFullscreen_ {};

	// = When in swapchain mode =
	// Currently viewed frame; i.e. the displayed commands in the command panel.
	// Might be different (newer) from the selected frame (CommandSelection) in case we force-updated
	// the commands due to not finding the selected command anymore but
	// wanting to show fresh commands.
	std::vector<FrameSubmission> frame_;
	u32 frameID_ {};
	// part of frame_
	FrameSubmission* submission_ {};

	// The currently selected record.
	IntrusivePtr<CommandRecord> record_ {};
	// Might be empty, signalling that no command is secleted.
	std::vector<const Command*> command_ {};

	std::unordered_set<const ParentCommand*> openedSections_;
	std::unordered_set<const FrameSubmission*> openedSubmissions_; // [swapchain] points into frame_
	std::unordered_set<const CommandRecord*> openedRecords_; // [swapchain] points into frame_[i].submissions

	// kept to avoid frequent allocations while matching/updating openedX_ sets
	struct {
		std::unordered_set<const ParentCommand*> openedSections;
		std::unordered_set<const FrameSubmission*> openedSubmissions;
		std::unordered_set<const CommandRecord*> openedRecords;
		std::unordered_set<const ParentCommand*> transitionedSections;
	} tmp_;

	// The commands to display
	CommandCategoryFlags commandFlags_ {};

	// Whether to only nest labels, supporting hierarchy-braking label nesting.
	// We currently enable it the first time we encounter such a record.
	bool brokenLabelNesting_ {};

	CommandViewer commandViewer_ {};

	bool showSingleSections_ {};
	UpdateTicker updateTick_ {};

	LinAllocator matchAlloc_;
	CommandSelection selector_;

	// TODO WIP experiments
	bool focusSelected_ {};
	bool freezeOnSparseBind_ {};

	MatchType defaultMatchType_ {MatchType::mixed};
};

} // namespace vil
