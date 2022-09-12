#pragma once

#include <fwd.hpp>
#include <command/record.hpp>
#include <gui/render.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/command.hpp>
#include <gui/update.hpp>
#include <gui/commandSelection.hpp>
#include <util/flags.hpp>

namespace vil {

struct FrameSubmission;
struct FrameMatch;

class CommandBufferGui {
public:
	CommandBufferGui() = default;
	~CommandBufferGui();

	void init(Gui& gui);

	void draw(Draw& draw);
	void destroyed(const Handle& handle);

	// swapchain only guaranteed to stay valid during call
	// TODO: somewhat misleading, will not consider the given swapchain but just
	// use dev.swapchain for updates. See todo on multi-swapchain support
	void showSwapchainSubmissions(Swapchain& swapchain);
	void select(IntrusivePtr<CommandRecord> record, Command* cmd = nullptr);
	void select(IntrusivePtr<CommandRecord> record, CommandBufferPtr cb);

	auto& commandViewer() { return commandViewer_; }
	auto& selector() { return selector_; }

private:
	void updateFromSelector();
	void updateCommandViewer(bool resetState);

	void displayFrameCommands(Swapchain&);
	void displayRecordCommands();
	void clearSelection(bool unselectCommandViewer);

	void updateRecord(IntrusivePtr<CommandRecord> record);
	void updateRecords(std::vector<FrameSubmission>);
	void updateRecords(const FrameMatch&, std::vector<FrameSubmission>&&);

private:
	friend class Gui;
	Gui* gui_ {};
	bool freezeCommands_ {};

	// = For selectionType_ swapchain =
	// Currently viewed frame; i.e. the displayed commands in the command panel.
	// Might be different (newer) from selectedFrame_ in case we force-updated
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

	// The commands to display
	CommandTypeFlags commandFlags_ {};

	// Whether to only nest labels, supporting hierarchy-braking label nesting.
	// We currently enable it the first time we encounter such a record.
	bool brokenLabelNesting_ {};

	CommandViewer commandViewer_ {};

	bool focusSelected_ {}; // TODO WIP experiment
	UpdateTicker updateTick_ {};

	LinAllocator matchAlloc_;

	// TODO: move to Gui class I guess
	CommandSelection selector_;
};

} // namespace vil
