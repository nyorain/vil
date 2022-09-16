#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/bufferViewer.hpp>
#include <gui/imageViewer.hpp>
#include <gui/shader.hpp>
#include <imgui/textedit.h>
#include <command/record.hpp>

namespace vil {

class CommandViewer {
public:
	enum class IOView {
		command, // view command information
		mesh, // vertex I/O
		ds, // descriptor sets
		pushConstants,
		attachment,
		transferSrc,
		transferDst,
		shader,
	};

	IOView view_;
	bool beforeCommand_ {}; // whether state is viewed before cmd
	bool showUnusedBindings_ {};

	union {
		struct {
			int selected; // for multidraw
		} command;

		struct {
			unsigned set;
			unsigned binding;
			unsigned elem;

			// image view state
			// buffer view state
			VkShaderStageFlagBits stage;
		} ds;

		struct {
			u32 index;
		} transfer;

		struct {
			bool output; // vertex input or output
		} mesh;

		struct {
			VkShaderStageFlagBits stage;
		} pushConstants;

		struct {
			AttachmentType type;
			unsigned id;
		} attachment;
	} viewData_;

public:
	CommandViewer();
	~CommandViewer();

	void init(Gui& gui);
	void draw(Draw& draw);

	void unselect();

	// When newState is nullptr, the old one should not be overwritten.
	void select(IntrusivePtr<CommandRecord>, std::vector<const Command*>,
		CommandDescriptorSnapshot, bool resetState,
		IntrusivePtr<CommandHookState> newState);

	CommandHookState* state() const { return state_.get(); }
	CommandRecord* record() const { return record_.get(); }
	const auto& command() const { return command_; }
	const CommandDescriptorSnapshot& dsState() const { return dsState_; }

	auto& vertexViewer() { return vertexViewer_; }

private:
	Device& dev() const;

	// NOTE: updateHook will acquire the device mutex internally, don't
	// call it when you have a mutex locked.
	void updateHook();
	void displayCommand();

	// IO list display
	void displayIOList();
	void displayTransferIOList();
	void displayDsList();

	// selected IO display
	void displaySelectedIO(Draw&);
	bool displayBeforeCheckbox();
	void displayDs(Draw&);
	void displayActionInspector(Draw&);
	void displayAttachment(Draw&);
	void displayPushConstants();
	void displayTransferData(Draw&);

	void displayVertexInput(Draw& draw, const DrawCmdBase&);
	void displayVertexOutput(Draw& draw, const DrawCmdBase&);
	void displayVertexViewer(Draw& draw);

	const PipelineShaderStage* displayDescriptorStageSelector(const Pipeline& pipe,
		unsigned setID, unsigned bindingID, VkDescriptorType dsType);

	// Can only be called once per frame
	void displayImage(Draw& draw, const CopiedImage& img);

private:
	friend class Gui;
	Gui* gui_ {};

	IntrusivePtr<CommandRecord> record_ {}; // the selected record
	std::vector<const Command*> command_ {}; // the selected command
	CommandDescriptorSnapshot dsState_ {};
	IntrusivePtr<CommandHookState> state_ {}; // the currently viewed state

	// For the one image we potentially display
	// DrawGuiImage ioImage_ {};

	VertexViewer vertexViewer_ {};
	BufferViewer bufferViewer_ {};
	ImageViewer imageViewer_ {};
	ShaderDebugger shaderDebugger_ {};

	bool doUpdateHook_ {};
};

} // namespace vil
