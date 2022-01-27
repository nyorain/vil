// This code is compiled into vil itself. But run in an integration
// test context, i.e. inside our application that correctly
// initializes everything.

#include <wrap.hpp>
#include <gui/gui.hpp>
#include <command/commands.hpp>
#include <gui/commandHook.hpp>
#include <layer.hpp>
#include <cb.hpp>
#include <rp.hpp>
#include <util/export.hpp>
#include <vk/dispatch_table_helper.h>
#include "./util.hpp"

using namespace tut;

namespace vil {

template<typename T, typename O>
T undispatch(O& dst) {
	std::shared_lock lock(dataMutex);
	for(auto& entry : dispatchableTable) {
		if(entry.second == &dst) {
			return u64ToHandle<T>(entry.first);
		}
	}

	throw std::runtime_error("Invalid handle");
}

void internalIntegrationTest(Device& dev) {
	// find queue
	auto& queue = *dev.queues.front();

	Setup stp;
	stp.dev = undispatch<VkDevice>(dev);;
	stp.queue = undispatch<VkQueue>(queue);
	stp.qfam = queue.family;
	layer_init_device_dispatch_table(stp.dev, &stp.dispatch, &vil::GetDeviceProcAddr);

	// setup texture
	auto tc = TextureCreation();
	auto tex = Texture(stp, tc);

	// setup render pass
	auto passes = {0u};
	auto format = tc.ici.format;
	auto rpi = renderPassInfo({{format}}, {{passes}});
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
	cpi.queueFamilyIndex = queue.family;
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
	auto& rec = *vilCB.record();
	auto dst = rec.commands->next->next;

	dev.commandHook->queryTime = true;
	dev.commandHook->forceHook = true;
	dev.commandHook->target.all = true;
	dev.commandHook->desc(vilCB.recordPtr(), {dst}, {});

	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb;
	QueueSubmit(stp.queue, 1u, &si, VK_NULL_HANDLE);

	DeviceWaitIdle(stp.dev);

	dlg_assert(dev.commandHook->completed.size() == 1u);

	// cleanup
	DestroyFramebuffer(stp.dev, fb, nullptr);
	DestroyRenderPass(stp.dev, rp, nullptr);
	DestroyCommandPool(stp.dev, cmdPool, nullptr);

	// init gui
	auto gui = std::make_unique<vil::Gui>();
	gui->init(dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
	// TODO: actually render gui stuff. Create own swapchain?
	//   or rather implement render-on-image for that?
	//   We could create a headless surface tho.
}

// TODO: write test where we record a command buffer that executes
// each command once. Then hook each of those commands, separately.

} // namespace vil

extern "C" VIL_EXPORT void vil_runInternalIntegrationTests() {
	// at this point, there should be an instance and device
	vil::Device* dev;

	{
		std::lock_guard lock(vil::dataMutex);
		dlg_assert(!vil::devByLoaderTable.empty());
		dev = vil::devByLoaderTable.begin()->second;
	}

	vil::internalIntegrationTest(*dev);
}
