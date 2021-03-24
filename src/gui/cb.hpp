#pragma once

#include <fwd.hpp>
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
	u32 swapchainCounter_ {};
	bool freezePresentBatches_ {};

	// The commands to display
	CommandTypeFlags commandFlags_ {};

	// The selected command (hierarchy) inside the cb.
	// Might be empty, signalling that no command is secleted.
	std::vector<const Command*> command_ {};

	// In case we have a selected command, we store its description inside
	// the CommandRecord here. This way we can (try to) find the logically
	// same command in future records/cb selections.
	std::vector<CommandDesc> desc_ {};

	CommandViewer commandViewer_ {};
};

} // namespace vil
