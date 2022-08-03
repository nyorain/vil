// This code is compiled into vil itself. But run in an integration
// test context, i.e. inside our application that correctly
// initializes everything.

#include <wrap.hpp>
#include <gui/gui.hpp>
#include <command/commands.hpp>
#include <command/desc.hpp>
#include <commandHook/hook.hpp>
#include <threadContext.hpp>
#include <layer.hpp>
#include <queue.hpp>
#include <cb.hpp>
#include <rp.hpp>
#include "./internal.hpp"

using namespace tut;

namespace vil::test {

TEST(int_basic) {
	auto& stp = gSetup;

	// setup texture
	auto tc = TextureCreation();
	auto tex = Texture(stp, tc);

	// setup render pass
	auto passes = {0u};
	auto format = tc.ici.format;
	auto rpi = renderPassInfo({{format}}, {{passes}});

	// add dummy VK_ATTACHMENT_UNUSED depth stencil attachment
	VkAttachmentReference unusedRef;
	unusedRef.attachment = VK_ATTACHMENT_UNUSED;
	unusedRef.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	rpi.subpasses[0].pDepthStencilAttachment = &unusedRef;

	VkRenderPass rp;
	VK_CHECK(CreateRenderPass(stp.dev, &rpi.info(), nullptr, &rp));

	// setup fb
	VkFramebuffer fb;
	VkFramebufferCreateInfo fbi = {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.attachmentCount = 1;
	fbi.pAttachments = &tex.imageView;
	fbi.renderPass = rp;
	fbi.width = tc.ici.extent.width;
	fbi.height = tc.ici.extent.height;
	fbi.layers = 1;
	VK_CHECK(CreateFramebuffer(stp.dev, &fbi, nullptr, &fb));

	// setup command pool & buffer
	VkCommandPoolCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = stp.qfam;
	VkCommandPool cmdPool;
	VK_CHECK(CreateCommandPool(stp.dev, &cpi, nullptr, &cmdPool));

	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 1u;
	cbai.commandPool = cmdPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer cb;
	VK_CHECK(AllocateCommandBuffers(stp.dev, &cbai, &cb));

	auto& vilCB = unwrap(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::initial);

	// record commands
	VkCommandBufferBeginInfo cbi {};
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(BeginCommandBuffer(cb, &cbi));

	// incorrect label hierarchy test
	VkDebugUtilsLabelEXT label {};
	label.pLabelName = "TestLabel1";
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;

	VkClearValue clearValue {};
	VkRenderPassBeginInfo rbi {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = rp;
	rbi.renderArea.extent.width = tc.ici.extent.width;
	rbi.renderArea.extent.height = tc.ici.extent.height;
	rbi.clearValueCount = 1u;
	rbi.pClearValues = &clearValue;
	rbi.framebuffer = fb;

	CmdBeginDebugUtilsLabelEXT(cb, &label);
	CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	CmdEndDebugUtilsLabelEXT(cb);
	CmdEndRenderPass(cb);

	// just pop labels that were never pushed - valid behavior per spec
	CmdEndDebugUtilsLabelEXT(cb);
	CmdEndDebugUtilsLabelEXT(cb);

	// other case of hierarchy mismatch
	// we just don't end it inside the scope
	label.pLabelName = "TestLabel2";
	CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	CmdBeginDebugUtilsLabelEXT(cb, &label);
	CmdEndRenderPass(cb);

	// have some unterminated labels
	label.pLabelName = "Unterminated1";
	CmdBeginDebugUtilsLabelEXT(cb, &label);
	label.pLabelName = "Unterminated2";
	CmdBeginDebugUtilsLabelEXT(cb, &label);

	EndCommandBuffer(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::executable);

	// submit it, make sure it's hooked
	auto& rec = *vilCB.lastRecordPtr();
	auto* cmd = rec.commands->children_;
	auto dst = cmd->next->next;

	auto& vilDev = *stp.vilDev;
	CommandHook::HookOps ops {};
	ops.queryTime = true;
	vilDev.commandHook->ops(std::move(ops));

	CommandHook::HookTarget target {};
	target.all = true;
	vilDev.commandHook->target(std::move(target));

	vilDev.commandHook->forceHook.store(true);
	vilDev.commandHook->desc(vilCB.lastRecordPtr(), {dst}, {});

	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb;
	QueueSubmit(stp.queue, 1u, &si, VK_NULL_HANDLE);

	DeviceWaitIdle(stp.dev);

	dlg_assert(vilDev.commandHook->moveCompleted().size() == 1u);

	// cleanup
	DestroyFramebuffer(stp.dev, fb, nullptr);
	DestroyRenderPass(stp.dev, rp, nullptr);
	DestroyCommandPool(stp.dev, cmdPool, nullptr);

	// init gui
	auto gui = std::make_unique<vil::Gui>();
	gui->init(vilDev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
	// TODO: actually render gui stuff. Create own swapchain?
	//   or rather implement render-on-image for that?
	//   We could create a headless surface tho.
}


TEST(int_secondary_cb) {
	auto& setup = gSetup;

	// matching of secondary command buffers
	// setup pool
	VkCommandPoolCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = setup.qfam;
	VkCommandPool cmdPool;
	VK_CHECK(CreateCommandPool(setup.dev, &cpi, nullptr, &cmdPool));

	// setup secondary cb
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 1u;
	cbai.commandPool = cmdPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	VkCommandBuffer scb;
	VK_CHECK(AllocateCommandBuffers(setup.dev, &cbai, &scb));

	{
		VkCommandBufferInheritanceInfo ii {};
		ii.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.pInheritanceInfo = &ii;
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
		VK_CHECK(BeginCommandBuffer(scb, &beginInfo));

		// some dummy label
		VkDebugUtilsLabelEXT label {};
		label.pLabelName = "TestLabel1";
		label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;

		CmdBeginDebugUtilsLabelEXT(scb, &label);
		CmdEndDebugUtilsLabelEXT(scb);

		EndCommandBuffer(scb);
	}

	// setup primary cbs
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 2u;
	VkCommandBuffer pcbs[2];
	VK_CHECK(AllocateCommandBuffers(setup.dev, &cbai, pcbs));

	for(auto& cb : pcbs) {
		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(BeginCommandBuffer(cb, &beginInfo));

		CmdExecuteCommands(cb, 1u, &scb);
		CmdExecuteCommands(cb, 1u, &scb);

		EndCommandBuffer(cb);
	}

	auto& cb0 = unwrap(pcbs[0]);
	auto& cb1 = unwrap(pcbs[1]);

	auto& rec0 = *cb0.lastRecordLocked();
	auto& rec1 = *cb1.lastRecordLocked();

	dlg_assert(rec0.commands->sectionStats().numChildSections == 2u);

	auto& execCmd0 = dynamic_cast<ExecuteCommandsCmd&>(*rec0.commands->firstChildParent());
	dlg_assert(execCmd0.sectionStats().numChildSections == 1u);
	dlg_assert(execCmd0.nextParent_);

	auto& childCmd0 = dynamic_cast<ExecuteCommandsChildCmd&>(*execCmd0.firstChildParent());
	dlg_assert(childCmd0.sectionStats().numChildSections == 1u);
	dlg_assert(childCmd0.firstChildParent());

	auto& execCmd1 = dynamic_cast<ExecuteCommandsCmd&>(*rec1.commands->firstChildParent());
	dlg_assert(execCmd1.sectionStats().numChildSections == 1u);
	dlg_assert(execCmd1.nextParent_);

	auto& childCmd1 = dynamic_cast<ExecuteCommandsChildCmd&>(*execCmd1.firstChildParent());
	dlg_assert(childCmd1.sectionStats().numChildSections == 1u);
	dlg_assert(childCmd1.firstChildParent());

	ThreadMemScope tms;
	auto res = match(tms, *rec0.commands, *rec1.commands);
	auto matcher = res.second;
	dlg_assert(matcher.match == matcher.total);
	dlg_assert(matcher.match > 0.f);

	auto secm = res.first;
	dlg_assert(secm.size() == 2u);
	dlg_assert(secm[1].a == &execCmd0 && secm[1].b == &execCmd1);
	dlg_assert(secm[1].children.size() == 1u);
	dlg_assert(secm[0].a == execCmd0.nextParent_ && secm[0].b == execCmd1.nextParent_);
	dlg_assert(secm[0].children.size() == 1u);

	{
		auto secm = res.first[1].children;
		dlg_assert(secm[0].a == &childCmd0 && secm[0].b == &childCmd1);
		dlg_assert(secm[0].children.size() == 1u); // for the label section
	}

	DestroyCommandPool(setup.dev, cmdPool, nullptr);
}

// TODO: write test where we record a command buffer that executes
// each command once. Then hook each of those commands, separately.

} // namespace vil::test
