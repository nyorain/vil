#include "overlay.hpp"
#include "swapchain.hpp"
#include "image.hpp"
#include "cb.hpp"
#include "sync.hpp"
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
		foundDraw->init(dev);
	}

	auto& draw = *foundDraw;

	VkCommandBufferBeginInfo cbBegin = vk::CommandBufferBeginInfo();
	VK_CHECK(dev.dispatch.vkBeginCommandBuffer(draw.cb, &cbBegin));

	gui.makeImGuiCurrent();
	ImGui::GetIO().DisplaySize.x = swapchain->ci.imageExtent.width;
	ImGui::GetIO().DisplaySize.y = swapchain->ci.imageExtent.height;

	// if there is a platform (for input stuff), update it
	// if(platform) {
	// 	platform->update();
	// }

	// render gui
	gui.draw(draw, false);
	auto& drawData = *ImGui::GetDrawData();

	renderer.uploadDraw(draw, drawData);

	// make sure all used resources are readable.
	auto selImg = gui.selectedImage();
	if(selImg && !gui.selectedImageViewable()) {
		selImg = nullptr;
	}

	std::lock_guard queueLock(dev.queueMutex);

	// TODO: device mutex should probably be unlocked for our
	// queue calls in the end. Care must be taken though!
	std::lock_guard devMutex(dev.mutex);

	VkImageLayout finalLayout;
	if(selImg) {
		auto& img = *selImg;
		finalLayout = img.pendingLayout;

		// find all submissions associated with this image
		std::vector<PendingSubmission*> toComplete;

		// We can implement this in two possible way: either check
		// for all command buffers using the handle whether they are
		// pending or check for all pending submissions whether they
		// use the handle.
#if 0
		for(auto& cb : img.refCbs) {
			for(auto* pending : cb->pending) {
				toComplete.push_back(pending);
			}
		}

		// make sure the range is unique. A pending submission might
		// be in here multiple times if it contains multiple distinct
		// command buffers using the image.
		auto newEnd = std::unique(toComplete.begin(), toComplete.end());

		// pre-check all submissions, whether they are done now
		auto checker = [](auto* pending) {
			return checkLocked(*pending);
		};
		newEnd = std::remove_if(toComplete.begin(), newEnd, checker);
		toComplete.erase(newEnd, toComplete.end());
#else
		for(auto it = dev.pending.begin(); it != dev.pending.end();) {
			auto& pending = *it;

			// remove finished pending submissions.
			// important to do this before accessing them.
			if(checkLocked(*pending)) {
				// don't increase iterator as the current one
				// was erased.
				continue;
			}

			bool wait = false;
			for(auto& sub : pending->submissions) {
				for(auto* cb : sub.cbs) {
					dlg_assert(cb->state == CommandBuffer::State::executable);
					auto it = cb->images.find(img.handle);
					if(it == cb->images.end()) {
						continue;
					}

					wait = true;
					break;
				}
			}

			if(wait) {
				toComplete.push_back(pending.get());
			}

			++it;
		}

#endif

		if(!toComplete.empty()) {
			std::vector<VkFence> fences;
			for(auto* pending : toComplete) {
				if(pending->appFence) {
					fences.push_back(pending->appFence->handle);
				} else {
					fences.push_back(pending->ourFence);
				}
			}

			VK_CHECK(dev.dispatch.vkWaitForFences(dev.handle,
				fences.size(), fences.data(), true, UINT64_MAX));

			for(auto* pending : toComplete) {
				auto res = checkLocked(*pending);
				// we waited for it above. It should really
				// be completed now.
				dlg_assert(res);
			}
		}

		// Make sure our image is in the right layout.
		// And we are allowed to read it
		VkImageMemoryBarrier imgb = vk::ImageMemoryBarrier();
		imgb.image = img.handle;
		imgb.subresourceRange = gui.selectedImageSubresourceRange();
		imgb.oldLayout = finalLayout;
		imgb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imgb.srcAccessMask = {}; // TODO: dunno. Track/figure out possible flags
		imgb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		// TODO: transfer queue.
		// We currently just force concurrent mode on image/buffer creation
		// but that might have performance impact.
		// Requires additional submissions to the other queues.
		// We should first check whether the queue is different in first place.
		// if(img.ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
		// }

		dev.dispatch.vkCmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // wait for everything
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
			0, 0, nullptr, 0, nullptr, 1, &imgb);
	}

	renderer.recordDraw(draw, swapchain->ci.imageExtent, buffers[imageIdx].fb,
		false, drawData);

	if(selImg) {
		// return it to original layout
		VkImageMemoryBarrier imgb = vk::ImageMemoryBarrier();
		imgb.image = selImg->handle;
		imgb.subresourceRange = gui.selectedImageSubresourceRange();
		imgb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imgb.newLayout = finalLayout;
		dlg_assert(
			finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
			finalLayout != VK_IMAGE_LAYOUT_UNDEFINED);
		imgb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		imgb.srcAccessMask = {}; // TODO: dunno. Track/figure out possible flags

		// TODO: transfer queue.
		// We currently just force concurrent mode on image/buffer creation
		// but that might have performance impact.
		// Requires additional submissions to the other queues.
		// We should first check whether the queue is different in first place.
		// if(selImg->ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
		// }

		dev.dispatch.vkCmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // wait in everything
			0, 0, nullptr, 0, nullptr, 1, &imgb);
	}

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
