#pragma once

#include <fwd.hpp>
#include <command/desc.hpp>
#include <command/record.hpp>
#include <gui/render.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/command.hpp>
#include <util/flags.hpp>
#include <command/desc.hpp>

// TODO: maybe we should just get rid of the non-swapchain update modes.
// They are unreliable and make this class a lot more complicated
// than it should be. The main usecase is swapchain-mode anyways.
// But not sure, maybe others find it useful.

namespace vil {

struct FrameSubmission;

class CommandBufferGui {
public:
	// Defines from which source the displayed commands are updated.
	enum class UpdateMode {
		none, // does not update them at all. Displays static record
		commandBuffer, // always displays current record of commandBuffer
		any, // always displays the last submission matching the selection
		swapchain, // displays all commands between two swapchain presents
	};

	enum class SelectionType {
		none,
		submission,
		record,
		command,
	};

public:
	CommandBufferGui() = default;
	~CommandBufferGui();

	void init(Gui& gui);

	void draw(Draw& draw);
	void destroyed(const Handle& handle);

	void showSwapchainSubmissions();
	void select(IntrusivePtr<CommandRecord> record);
	void select(IntrusivePtr<CommandRecord> record, CommandBuffer& cb);

	auto& commandViewer() { return commandViewer_; }

private:
	void updateState();
	void updateHookTarget();

	void displayFrameCommands();
	void displayRecordCommands();
	void clearSelection();

	void updateRecord(IntrusivePtr<CommandRecord> record);
	void updateRecords(std::vector<FrameSubmission>, bool updateSelection);
	void updateRecords(const MatchResult&, std::vector<FrameSubmission>);

private:
	friend class Gui;
	Gui* gui_ {};

	UpdateMode mode_ {};
	CommandBuffer* cb_ {}; // when updating from cb

	// The command record we are currently viewing.
	// We make sure it stays alive. In swapchain mode, this is nullptr
	// if we don't have a selected record, otherwise it's the same as
	// selectedRecord_ (TODO: kinda redundant, merge with selectedRecord_).
	IntrusivePtr<CommandRecord> record_ {};

	// For swapchain
	std::vector<FrameSubmission> records_; // currently viewed frame; i.e. the displayed commands
	u32 swapchainPresent_ {}; // present id of last time we got a completed hook
	bool freezeCommands_ {};
	bool freezeState_ {};

	SelectionType selectionType_ {};

	// Potentially old, selected command, in its record and its batch.
	// [Swapchain mode] Batch of command viewer; for matching
	std::vector<FrameSubmission> selectedFrame_;
	// [Swapchain mode] part of selectedFrame_
	FrameSubmission* selectedBatch_ {};
	// In swapchain mode: part of selectedBatch_
	IntrusivePtr<CommandRecord> selectedRecord_ {};
	// The selected command (hierarchy) inside selectedRecord_.
	// Might be empty, signalling that no command is secleted.
	// Only valid if selectionType_ == command.
	std::vector<const Command*> selectedCommand_ {};

	std::unordered_set<const ParentCommand*> openedSections_;

	// The commands to display
	CommandTypeFlags commandFlags_ {};

	// Whether to only nest labels, supporting hierarchy-braking label nesting.
	// We currently enable it the first time we encounter such a record.
	bool brokenLabelNesting_ {};

	CommandViewer commandViewer_ {};

	bool focusSelected_ {}; // TODO WIP experiment
};

} // namespace vil
