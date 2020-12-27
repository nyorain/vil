#pragma once

#include <device.hpp>
#include <commandDesc.hpp>

namespace fuen {

struct CommandBufferGui {
	void draw();
	// void select(CommandBuffer& cb);
	void select(CommandBufferGroup& group);
	void destroyed(const Handle& handle);

	CommandBufferGui();
	~CommandBufferGui();

	Gui* gui_ {};
	// CommandBuffer* cb_ {}; // the selected command buffer

	CommandRecord* record_ {};
	CommandBufferGroup* group_ {};
	const Command* command_ {}; // the selected command inside the cb
	// u32 recordID_ {}; // the recordID of the last shown cb record

	std::vector<CommandDesc> desc_ {};

	// Hooking the command buffer means replacing it
	struct {
		VkCommandPool commandPool {};
		VkCommandBuffer cb {};
		bool needsUpdate {true};
		u32 qf {};

		bool query {};
		VkQueryPool queryPool {};
		VkPipelineStageFlagBits queryStart {};
		VkPipelineStageFlagBits queryEnd {};
	} hooked_;

	// private
	VkCommandBuffer cbHook(CommandBuffer& cb);
	void hookRecord(const Command* cmd);
	// void hookRecord(const CommandVector<CommandPtr>& commands);
	// void hookRecord(const std::vector<CommandPtr>& commands);
};

} // namespace fuen
