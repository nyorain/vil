#include <command/desc.hpp>
#include <command/record.hpp>
#include <command/commands.hpp>
#include <vk/vulkan.h>
#include "../bugged.hpp"
#include "../approx.hpp"

using namespace vil;

TEST(unit_command_match_barrier) {
	BarrierCmd b1;
	b1.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	b1.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	auto b2 = b1;

	EXPECT(eval(b1.match(b2)), approx(1.f));
	EXPECT(eval(b1.match(b2)), eval(b2.match(b1)));

	b1.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	EXPECT(eval(b1.match(b2)) < 1.f, true);

	WaitEventsCmd waitCmd;
	EXPECT(waitCmd.match(b2).match, 0.f);
	EXPECT(eval(waitCmd.match(b2)), 0.f);
}

