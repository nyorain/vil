#include "overlay.hpp"
#include "swapchain.hpp"
#include "image.hpp"
#include <vkpp/names.hpp>
#include <imgui/imgui.h>

namespace fuen {

// Overlay
void Overlay::init(Swapchain& swapchain) {
	this->swapchain = &swapchain;
	// this->platform = findData<Platform>(swapchain.ci.surface);
	this->renderer.init(*swapchain.dev, swapchain.ci.imageFormat, false);

}

void Overlay::initRenderBuffers() {
	auto& swapchain = *this->swapchain;
	auto& dev = *swapchain.dev;

	buffers.clear(); // remove old
	buffers.resize(swapchain.images.size());
	for(auto i = 0u; i < swapchain.images.size(); ++i) {
		buffers[i].init(dev, swapchain.images[i]->handle,
			swapchain.ci.imageFormat, swapchain.ci.imageExtent, renderer.rp());
	}
}

// TODO: this needs a rework. The gui might display an image that is
// currently written to from another submission (possibly on a different
// queue, we have to care about queue ownership).
// Should that logic better be in Renderer tho?
VkResult Overlay::drawPresent(Queue& queue, span<const VkSemaphore> semaphores,
		u32 imageIdx) {
	auto& dev = *queue.dev;

	// find free draw
	Draw* foundDraw = nullptr;
	for(auto& draw : draws) {
		if(dev.dispatch.vkGetFenceStatus(dev.handle, draw.fence) == VK_SUCCESS) {
			VK_CHECK(dev.dispatch.vkResetFences(dev.handle, 1, &draw.fence));

			foundDraw = &draw;
			break;
		}
	}

	if(!foundDraw) {
		foundDraw = &draws.emplace_back();
		*foundDraw = {};
		foundDraw->init(dev);
	}

	auto& draw = *foundDraw;

	VkCommandBufferBeginInfo cbBegin = vk::CommandBufferBeginInfo();
	VK_CHECK(dev.dispatch.vkBeginCommandBuffer(draw.cb, &cbBegin));

	gui.makeImGuiCurrent();
	ImGui::GetIO().DisplaySize.x = swapchain->ci.imageExtent.width;
	ImGui::GetIO().DisplaySize.y = swapchain->ci.imageExtent.height;

	// TODO: make sure all used resources are readable.

	// if there is a platform (for input stuff), update it
	// if(platform) {
	// 	platform->update();
	// }

	// render gui
	gui.draw(draw, false);
	auto& drawData = *ImGui::GetDrawData();

	renderer.uploadDraw(draw, drawData);
	renderer.recordDraw(draw, swapchain->ci.imageExtent, buffers[imageIdx].fb,
		false, drawData);

	dev.dispatch.vkEndCommandBuffer(draw.cb);

	// submit batch
	// TODO: handle case where application doesn't give us semaphore
	// (and different queues are used?)
	auto waitStages = std::make_unique<VkPipelineStageFlags[]>(semaphores.size());
	for(auto i = 0u; i < semaphores.size(); ++i) {
		waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}

	// TODO: we could add dev.resetSemaphores here as wait semaphores.
	// And move their vector into Draw. And when the draw fence is ready,
	// move them back into the semaphore pool.

	// TODO: wait upon all submissions using resources we use to complete.
	// (technically only resources that potentially write them).

	VkSubmitInfo submitInfo = vk::SubmitInfo();
	submitInfo.commandBufferCount = 1u;
	submitInfo.pCommandBuffers = &draw.cb;
	submitInfo.signalSemaphoreCount = 1u;
	submitInfo.pSignalSemaphores = &draw.semaphore;
	submitInfo.pWaitDstStageMask = waitStages.get();
	submitInfo.pWaitSemaphores = semaphores.data();
	submitInfo.waitSemaphoreCount = semaphores.size();

	auto res = dev.dispatch.vkQueueSubmit(dev.gfxQueue->queue, 1u, &submitInfo, draw.fence);
	if(res != VK_SUCCESS) {
		dlg_error("vkSubmit error: {}", vk::name(vk::Result(res)));
		return res;
	}

	// call down
	VkPresentInfoKHR presentInfo = vk::PresentInfoKHR();
	presentInfo.pImageIndices = &imageIdx;
	presentInfo.pWaitSemaphores = &draw.semaphore;
	presentInfo.waitSemaphoreCount = 1u;
	presentInfo.pSwapchains = &swapchain->handle;
	presentInfo.swapchainCount = 1u;
	// TODO: might be bad to not forward this
	// pi.pNext

	return dev.dispatch.vkQueuePresentKHR(queue.queue, &presentInfo);
}

} // namespace fuen
