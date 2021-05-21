#pragma once

#include <fwd.hpp>
#include <command/desc.hpp>
#include <command/record.hpp>
#include <gui/render.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/command.hpp>
#include <util/flags.hpp>
#include <command/desc.hpp>

namespace vil {

struct RecordBatch;

class CommandBufferGui {
public:
	// Defines from which source the displayed commands are updated.
	enum class UpdateMode {
		none, // does not update them at all. Displays static record
		commandBuffer, // always displays current record of commandBuffer
		any, // always displays the last submission matching the selection
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
	bool freezePresentBatches_ {};

	// The commands to display
	CommandTypeFlags commandFlags_ {};

	// The selected command (hierarchy) inside the cb.
	// Might be empty, signalling that no command is secleted.
	std::vector<const Command*> command_ {};
	CommandDescriptorSnapshot dsState_ {};

	CommandViewer commandViewer_ {};
};

} // namespace vil
