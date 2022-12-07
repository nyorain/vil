#include <gui/render.hpp>
#include <gui/gui.hpp>
#include <util/util.hpp>
#include <device.hpp>
#include <queue.hpp>
#include <imgui/imgui.h>
#include <vk/format_utils.h>
#include <commandHook/state.hpp>

namespace vil {

// Draw
Draw::Draw(Gui& gui, VkCommandPool commandPool) {
	auto& dev = gui.dev();
	this->dev = &dev;

	// init data
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 3u;
	cbai.commandPool = commandPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer cbs[3];
	VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &cbai, cbs));

	this->cb = cbs[0];
	this->cbLockedPre = cbs[1];
	this->cbLockedPost = cbs[2];

	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, this->cb);
	dev.setDeviceLoaderData(dev.handle, this->cbLockedPre);
	dev.setDeviceLoaderData(dev.handle, this->cbLockedPost);
	nameHandle(dev, this->cb, "Draw:cb");
	nameHandle(dev, this->cbLockedPre, "Draw:cbLockedPre");
	nameHandle(dev, this->cbLockedPost, "Draw:cbLockedPost");

	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateFence(dev.handle, &fci, nullptr, &fence));
	nameHandle(dev, this->fence, "Draw:fence");

	VkSemaphoreCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &presentSemaphore));
	nameHandle(dev, this->presentSemaphore, "Draw:presentSemaphore");

	if(!dev.timelineSemaphores) {
		VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &futureSemaphore));
		nameHandle(dev, this->futureSemaphore, "Draw:futureSemaphore");
	}
}

Draw::~Draw() {
	if(!dev) {
		return;
	}

	if(fence) {
		if(inUse) {
			dev->dispatch.WaitForFences(dev->handle, 1, &fence, true, UINT64_MAX);
		}

		dev->dispatch.DestroyFence(dev->handle, fence, nullptr);
	}

	dev->dispatch.DestroySemaphore(dev->handle, presentSemaphore, nullptr);
	dev->dispatch.DestroySemaphore(dev->handle, futureSemaphore, nullptr);

	// NOTE: could return them to the device pool alternatively
	for(auto sem : waitedUpon) {
		dev->dispatch.DestroySemaphore(dev->handle, sem, nullptr);
	}

	// We rely on the commandPool being freed to implicitly free this
	// command buffer. Since the gui owns the command pool this isn't
	// a problem.
	// dev->dispatch.FreeCommandBuffers(dev->handle, commandPool, ...)
}

// RenderBuffer
void RenderBuffer::init(Device& dev, VkImage img, VkFormat format,
		VkExtent2D extent, VkRenderPass rp, VkImageView depthView) {
	this->dev = &dev;
	this->image = img;

	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.components = {};
	ivi.format = format;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.layerCount = 1;
	ivi.subresourceRange.levelCount = 1;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &view));
	nameHandle(dev, this->view, "RenderBuffer:view");

	dlg_assert(depthView);
	VkImageView atts[] = {view, depthView};

	VkFramebufferCreateInfo fbi {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.attachmentCount = 2u;
	fbi.pAttachments = atts;
	fbi.layers = 1u;
	fbi.width = extent.width;
	fbi.height = extent.height;
	fbi.renderPass = rp;
	VK_CHECK(dev.dispatch.CreateFramebuffer(dev.handle, &fbi, nullptr, &fb));
	nameHandle(dev, this->fb, "RenderBuffer:fb");
}

RenderBuffer::~RenderBuffer() {
	if(!dev) {
		return;
	}

	if(fb) {
		dev->dispatch.DestroyFramebuffer(dev->handle, fb, nullptr);
	}
	if(view) {
		dev->dispatch.DestroyImageView(dev->handle, view, nullptr);
	}
}

} // namespace vil
