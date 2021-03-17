#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <gui/vertexViewer.hpp>

namespace vil {

// XXX: just a concept atm

class CommandViewer {
public:
	enum class IOView {
		command, // view command information
		mesh, // vertex I/O
		ds, // descriptor sets
		pushConstants,
		colorAttachment,
		depthStencilAttachment,
		transferSrc,
		transferDst,
	};

	IOView view;
	bool beforeCommand {}; // whether state is viewed before cmd

	union {
		struct {
			unsigned set;
			unsigned binding;
			unsigned elem;

			// image view state
			// buffer view state
		} ds;

		struct {
			bool input; // vertex input or output
		} mesh;

		struct {
			VkShaderStageFlagBits stage;
		} pushConstants;

		struct {
			unsigned id;
		} colorAttachment;
	} viewData;

public:
	void command(IntrusivePtr<CommandRecord>, Command&);
	void state(IntrusivePtr<CommandHookState>);

private:
	Gui* gui_ {};

	IntrusivePtr<CommandRecord> record_ {}; // the selected record
	Command* command_ {}; // the selected command
	IntrusivePtr<CommandHookState> state_ {}; // the currently viewed state

	VertexViewer vertexViewer_ {};
};

} // namespace vil
