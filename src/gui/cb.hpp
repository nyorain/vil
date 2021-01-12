#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <util/flags.hpp>
#include <commandDesc.hpp>

namespace fuen {

struct CopiedImage;

struct CommandBufferGui {
	void draw(Draw& draw);
	void select(IntrusivePtr<CommandRecord> record, bool updateFromGroup);
	void destroyed(const Handle& handle);

	// TODO: quite hacky, can only be called once per frame.
	void displayImage(const CopiedImage& img);
	Draw* draw_ {};

	CommandBufferGui();
	~CommandBufferGui();

	Gui* gui_ {};

	bool updateFromGroup_ {};
	CommandTypeFlags commandFlags_ {};

	// The command record we are currently viewing.
	// We keep it alive.
	IntrusivePtr<CommandRecord> record_ {};

	// The selected command inside the cb, might be null.
	const Command* command_ {};
	// In case we have a selected command, we store its description inside
	// the CommandRecord here. This way we can (try to) find the logically
	// same command in future records/cb selections.
	std::vector<CommandDesc> desc_ {};

	bool queryTime_ {};

	CommandHook* hook_ {}; // TODO: we can't know for sure it remains valid. Should probably be owned here
	DrawGuiImage imgDraw_ {};
};

} // namespace fuen
