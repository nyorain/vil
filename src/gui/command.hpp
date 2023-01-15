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

	bool beforeCommand_ {}; // whether state is viewed before cmd
	bool showBeforeCheckbox_ {};
	bool showUnusedBindings_ {};

	// We don't really want to update hook options immediately while drawing
	// the gui when something is pressed since that might mess with
	// local assumptions of the function we are in.
	// We therefore wait until we are finished with drawing the ui.
	bool doUpdateHook_ {};

public:
	CommandViewer();
	~CommandViewer();

	void init(Gui& gui);
	void draw(Draw& draw, bool skipList);

	void unselect();
	void updateFromSelector(bool forceUpdateHook);

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
	void displayActionInspector(Draw&, bool skipList);
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

	CommandSelection& selection() const;

private:
	Gui* gui_ {};

	VertexViewer vertexViewer_ {};
	BufferViewer bufferViewer_ {};
	ImageViewer imageViewer_ {};
	ShaderDebugger shaderDebugger_ {};

	// the currently viewed command hierarchy
	IntrusivePtr<CommandRecord> record_ {};
	std::vector<const Command*> command_ {};

	IOView view_ {};
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
};

} // namespace vil
