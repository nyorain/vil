#include <overlay.hpp>
#include <swapchain.hpp>
#include <image.hpp>
#include <cb.hpp>
#include <queue.hpp>
#include <sync.hpp>
#include <platform.hpp>
#include <imgui/imgui.h>
#include <util/dlg.hpp>
#include <util/util.hpp>

namespace vil {

// Overlay
Overlay::Overlay() = default;

Overlay::~Overlay() {
	buffers.clear();
	destroyDepth();
}

void Overlay::init(Swapchain& swapchain) {
	this->swapchain = &swapchain;
	this->gui = &swapchain.dev->getOrCreateGui(swapchain.ci.imageFormat);
	initRenderBuffers();
}

void Overlay::destroyDepth() {
	auto& dev = *swapchain->dev;
	dev.dispatch.DestroyImageView(dev.handle, depthView_, nullptr);
	dev.dispatch.DestroyImage(dev.handle, depthImage_, nullptr);
	dev.dispatch.FreeMemory(dev.handle, depthMemory_, nullptr);
}

void Overlay::initRenderBuffers() {
	gui->waitForDraws();

	auto& swapchain = *this->swapchain;
	auto& dev = *swapchain.dev;

	// destroy old buffers
	buffers.clear();
	destroyDepth();

	// create new
	// depth
	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.extent = {swapchain.ci.imageExtent.width, swapchain.ci.imageExtent.height, 1u};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.usage = /*VK_IMAGE_USAGE_SAMPLED_BIT |*/ VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.format = gui->depthFormat();

	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &depthImage_));
	nameHandle(dev, depthImage_, "overlayDepth");

	VkMemoryRequirements memReqs;
	dev.dispatch.GetImageMemoryRequirements(dev.handle, depthImage_, &memReqs);

	VkMemoryAllocateInfo mai {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.memoryTypeIndex = findLSB(dev.deviceLocalMemTypeBits & memReqs.memoryTypeBits);
	mai.allocationSize = memReqs.size;
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &mai, nullptr, &depthMemory_));
	nameHandle(dev, depthMemory_, "overlayDepthMemory");

	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, depthImage_, depthMemory_, 0u));

	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = depthImage_;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.format = gui->depthFormat();
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	ivi.subresourceRange.layerCount = 1u;
	ivi.subresourceRange.levelCount = 1u;

	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &depthView_));
	nameHandle(dev, depthView_, "overlayDepthView");

	// render buffers
	buffers.resize(swapchain.images.size());
	for(auto i = 0u; i < swapchain.images.size(); ++i) {
		buffers[i].init(dev, swapchain.images[i]->handle,
			swapchain.ci.imageFormat, swapchain.ci.imageExtent, gui->rp(), depthView_);
	}
}

VkResult Overlay::drawPresent(Queue& queue, span<const VkSemaphore> semaphores,
		u32 imageIdx) {
	dlg_assert(imageIdx < buffers.size());

	Gui::FrameInfo frameInfo {};
	frameInfo.extent = swapchain->ci.imageExtent;
	frameInfo.imageIdx = imageIdx;
	frameInfo.waitSemaphores = semaphores;
	frameInfo.fb = buffers[imageIdx].fb;
	frameInfo.fullscreen = false;
	frameInfo.clear = false;
	frameInfo.presentQueue = queue.handle;
	frameInfo.swapchain = swapchain->handle;
	frameInfo.image = buffers[imageIdx].image;

	return gui->renderFrame(frameInfo);
}

bool Overlay::compatible(const VkSwapchainCreateInfoKHR& a,
		const VkSwapchainCreateInfoKHR& b) {
	return a.imageFormat == b.imageFormat;
}

} // namespace vil
