#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <gui/vertexViewer.hpp>
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
	};

	IOView view_;
	bool beforeCommand_ {}; // whether state is viewed before cmd

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
		} ds;

		struct {
			bool output; // vertex input or output
		} mesh;

		struct {
			VkShaderStageFlagBits stage;
		} pushConstants;

		struct {
			unsigned id; // color attachment id
		} attachment;
	} viewData_;

public:
	CommandViewer();
	~CommandViewer();

	void init(Gui& gui);
	void draw(Draw& draw);

	void unselect();
	void select(IntrusivePtr<CommandRecord>, const Command&,
		CommandDescriptorSnapshot, bool resetState);
	void state(IntrusivePtr<CommandHookState>);

	CommandHookState* state() const { return state_.get(); }

	auto& vertexViewer() { return vertexViewer_; }

private:
	Device& dev() const;

	void updateHook();
	void displayCommand();

	// IO list display
	void displayIOList();
	void displayTransferIOList();
	void displayDsList();

	// selected IO display
	void displaySelectedIO(Draw&);
	void displayBeforeCheckbox();
	void displayDs(Draw&);
	void displayActionInspector(Draw&);
	void displayAttachment(Draw&);
	void displayPushConstants();
	void displayTransferData(Draw&);

	void displayVertexInput(Draw& draw, const DrawCmdBase&);
	void displayVertexOutput(Draw& draw, const DrawCmdBase&);
	void displayVertexViewer(Draw& draw);

	// Can only be called once per frame
	void displayImage(Draw& draw, const CopiedImage& img);

private:
	friend class Gui;
	Gui* gui_ {};

	IntrusivePtr<CommandRecord> record_ {}; // the selected record
	const Command* command_ {}; // the selected command
	CommandDescriptorSnapshot dsState_ {};
	IntrusivePtr<CommandHookState> state_ {}; // the currently viewed state

	// For the one image we potentially display
	DrawGuiImage ioImage_ {};

	VertexViewer vertexViewer_ {};
};

} // namespace vil
