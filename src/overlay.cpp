#include <overlay.hpp>
#include <swapchain.hpp>
#include <image.hpp>
#include <cb.hpp>
#include <queue.hpp>
#include <sync.hpp>
#include <platform.hpp>
#include <dlg/dlg.hpp>
#include <imgui/imgui.h>

namespace fuen {

// Overlay
Overlay::Overlay() = default;
Overlay::~Overlay() = default;

void Overlay::init(Swapchain& swapchain) {
	this->swapchain = &swapchain;
	this->gui.init(*swapchain.dev, swapchain.ci.imageFormat, false);
	initRenderBuffers();
}

void Overlay::initRenderBuffers() {
	gui.finishDraws();

	auto& swapchain = *this->swapchain;
	auto& dev = *swapchain.dev;

	buffers.clear(); // remove old
	buffers.resize(swapchain.images.size());
	for(auto i = 0u; i < swapchain.images.size(); ++i) {
		buffers[i].init(dev, swapchain.images[i]->handle,
			swapchain.ci.imageFormat, swapchain.ci.imageExtent, gui.rp());
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
	frameInfo.presentQueue = queue.handle;
	frameInfo.swapchain = swapchain->handle;

	auto res = gui.renderFrame(frameInfo);
	return res.result;
}

bool Overlay::compatible(const VkSwapchainCreateInfoKHR& a,
		const VkSwapchainCreateInfoKHR& b) {
	return a.imageFormat == b.imageFormat;
}

} // namespace fuen
