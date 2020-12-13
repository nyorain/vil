#pragma once

#include <device.hpp>

namespace fuen {

struct CommandBufferGui {
	void draw();
	void select(CommandBuffer& cb);
	void destroyed(const Handle& handle);

	Gui* gui_ {};
	CommandBuffer* cb_ {}; // the selected command buffer
	const Command* command_ {}; // the selected command inside the cb
	u32 resetCount_ {}; // the resetCount of cb at which teh command was valid

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
	void hookRecord(span<const std::unique_ptr<Command>> commands);
};

} // namespace fuen
