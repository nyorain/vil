#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <util/flags.hpp>
#include <commandDesc.hpp>

namespace fuen {

struct CopiedImage;
struct RecordBatch;

struct CommandBufferGui {
public:
	// Defines from which source the displayed commands are updated.
	enum class UpdateMode {
		none, // does not update them at all. Displays static record
		commandBuffer, // always displays current record of commandBuffer
		commandGroup, // always displayed last record of command group
		swapchain, // displays all commands between two swapchain presents
	};

public:
	CommandBufferGui();
	~CommandBufferGui();

	void draw(Draw& draw);

	void showSwapchainSubmissions();
	void select(IntrusivePtr<CommandRecord> record);
	void select(IntrusivePtr<CommandRecord> record, CommandBuffer& cb);
	void selectGroup(IntrusivePtr<CommandRecord> record);

	void destroyed(const Handle& handle);

	bool displayActionInspector(const Command&);
	void displayDs(const Command&);

	// TODO: hacky, can only be called once per frame.
	void displayImage(const CopiedImage& img);

public: // TODO: make private
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

	// HACKY SECTION OF SHAME
	DrawGuiImage ioImage_ {}; // TODO

	// TODO hacky. See https://github.com/ocornut/imgui/issues/1655
	bool columnWidth0_ {};
	bool columnWidth1_ {};
	Draw* draw_ {}; // TODO
};

} // namespace fuen
