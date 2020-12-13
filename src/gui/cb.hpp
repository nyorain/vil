#pragma once

#include <device.hpp>
#include <commands.hpp> // TODO!! vector<CommandDescription> should work ffs

namespace fuen {

struct CommandBufferGui {
	void draw();
	void select(CommandBuffer& cb);
	void destroyed(const Handle& handle);

	CommandBufferGui();
	~CommandBufferGui();

	Gui* gui_ {};
	CommandBuffer* cb_ {}; // the selected command buffer
	const Command* command_ {}; // the selected command inside the cb
	u32 resetCount_ {}; // the resetCount of cb at which teh command was valid

	std::vector<CommandDescription> desc_ {};

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
