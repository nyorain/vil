#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <gui/vertexViewer.hpp>
#include <util/flags.hpp>
#include <commandDesc.hpp>

namespace vil {

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
	void displayDsList(const Command&);
	void displayIOList(const Command&);
	void displayVertexViewer(Draw& draw, const Command&);
	void displaySelectedIO(Draw& draw, const Command&);
	void displayInspector(Draw& draw, const Command&);
	void displayActionInspector(Draw& cmd, const Command&);
	void displayDs(Draw& draw, const Command&);

	// Can only be called once per frame.
	void displayImage(Draw& draw, const CopiedImage& img);

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

	// For the one image we potentially display
	DrawGuiImage ioImage_ {};

	VertexViewer vertexViewer_ {};

	struct VertexDrawData {
		CommandBufferGui* self;
		VkCommandBuffer cb;
		Vec2f offset;
		Vec2f size;
		const DrawCmdBase* cmd;
	} vertexDrawData_;
};

} // namespace vil
