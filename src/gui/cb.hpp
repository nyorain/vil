#pragma once

#include <fwd.hpp>
#include <commandDesc.hpp>
#include <gui/render.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/command.hpp>
#include <util/flags.hpp>
#include <commandDesc.hpp>

namespace vil {

struct RecordBatch;

class CommandBufferGui {
public:
	// Defines from which source the displayed commands are updated.
	enum class UpdateMode {
		none, // does not update them at all. Displays static record
		commandBuffer, // always displays current record of commandBuffer
		commandGroup, // always displayed last record of command group
		swapchain, // displays all commands between two swapchain presents
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
	void selectGroup(IntrusivePtr<CommandRecord> record);

private:
	void updateHookTarget();

private:
	friend class Gui;
	Gui* gui_ {};

	UpdateMode mode_ {};
	CommandBuffer* cb_ {}; // when updating from cb

	// The command record we are currently viewing.
	// We make sure it stays alive.
	IntrusivePtr<CommandRecord> record_ {};

	// For swapchain
	std::vector<RecordBatch> records_;
	u32 swapchainCounter_ {}; // counter of last processed batch
	// How many records of same group come before target.
	// We need this in case there are multiple records of the same group
	// in a single frame.
	// TODO: this is not very stable. It can break for instance if the
	// number of submissions belonging to the same group changes between
	// frames.
	// We should:
	// - improve the matching/grouping algorithm. Especially make groups a better
	//   representation of the actual records.
	// - maybe still select the *best* match when there are multiple submission
	//   of same group in a frame. This matching can include submission order
	//   (what we do here exclusively) but can use additional information.
	u32 groupCounter_ {};
	bool freezePresentBatches_ {};

	// The commands to display
	CommandTypeFlags commandFlags_ {};

	// The selected command (hierarchy) inside the cb.
	// Might be empty, signalling that no command is secleted.
	std::vector<const Command*> command_ {};
	CommandDescriptorState dsState_ {};

	CommandViewer commandViewer_ {};
};

} // namespace vil
