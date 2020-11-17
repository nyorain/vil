#pragma once

#include "fwd.hpp"
#include "syncedMap.hpp"
#include "handle.hpp"

#include <vkpp/dispatch.hpp>
#include <vkpp/span.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <shared_mutex>
#include <memory>

namespace fuen {

struct Submission {
	std::vector<std::pair<VkSemaphore, VkPipelineStageFlags>> waitSemaphores;
	std::vector<VkSemaphore> signalSemaphore;

	// Accessing those might not be save if the submission has completed,
	// they might have already been destroyed. So:
	// - lock dev.mutex (shared lock is enough)
	// - check whether associated pending fence has completed
	// - only if not are you allowed to acces them (and only while
	//   mutex is still locked)
	std::vector<CommandBuffer*> cbs;
};

struct PendingSubmission {
	Queue* queue {};
	std::vector<Submission> submissions;

	// The fence added by the caller.
	// Might be null
	Fence* appFence {};

	// When the caller didn't add a fence, we added this one from the fence pool.
	// When appFence is not null, this is null.
	VkFence ourFence {};
	std::mutex ourMutex;
};

// Expects dev.mutex to be locked
bool checkLocked(PendingSubmission& subm);

struct Device {
	Instance* ini;
	VkDevice handle;
	VkPhysicalDevice phdev;
	vk::DynamicDispatch dispatch;

	PFN_vkSetDeviceLoaderData setDeviceLoaderData;

	std::vector<std::unique_ptr<Queue>> queues;
	Queue* gfxQueue;

	VkDescriptorPool dsPool;
	VkSampler sampler;
	VkDescriptorSetLayout dsLayout;
	VkPipelineLayout pipeLayout;
	VkCommandPool commandPool;

	u32 hostVisibleMemTypeBits;
	u32 deviceLocalMemTypeBits;

	std::unique_ptr<DisplayWindow> window;

	std::vector<VkFence> fencePool; // currently unused fences
	std::vector<std::unique_ptr<PendingSubmission>> pending;

	// Mutex for general shared access.
	// While this mutex is locked, resources won't be inserted or
	// erased from the resource tables below (and therefore can't
	// logically be created or destroyed).
	std::shared_mutex mutex;
	std::mutex queueMutex; // mutex for accessing queues

	SyncedUniqueUnorderedMap<VkSwapchainKHR, Swapchain> swapchains;

	SyncedUniqueUnorderedMap<VkImage, Image> images;
	SyncedUniqueUnorderedMap<VkImageView, ImageView> imageViews;
	SyncedUniqueUnorderedMap<VkSampler, Sampler> samplers;
	SyncedUniqueUnorderedMap<VkBuffer, Buffer> buffers;
	SyncedUniqueUnorderedMap<VkPipelineLayout, PipelineLayout> pipeLayouts;
	SyncedUniqueUnorderedMap<VkPipeline, ComputePipeline> computePipes;
	SyncedUniqueUnorderedMap<VkPipeline, GraphicsPipeline> graphicsPipes;
	SyncedUniqueUnorderedMap<VkFramebuffer, Framebuffer> framebuffers;
	SyncedUniqueUnorderedMap<VkRenderPass, RenderPass> renderPasses;
	SyncedUniqueUnorderedMap<VkCommandPool, CommandPool> commandPools;
	SyncedUniqueUnorderedMap<VkCommandBuffer, CommandBuffer> commandBuffers;
	SyncedUniqueUnorderedMap<VkFence, Fence> fences;
	SyncedUniqueUnorderedMap<VkDescriptorPool, DescriptorPool> dsPools;
	SyncedUniqueUnorderedMap<VkDescriptorSetLayout, DescriptorSetLayout> dsLayouts;
	SyncedUniqueUnorderedMap<VkDescriptorSet, DescriptorSet> descriptorSets;
	SyncedUniqueUnorderedMap<VkShaderModule, ShaderModule> shaderModules;
	SyncedUniqueUnorderedMap<VkDeviceMemory, DeviceMemory> deviceMemories;
	SyncedUniqueUnorderedMap<VkEvent, Event> events;
	SyncedUniqueUnorderedMap<VkSemaphore, Semaphore> semaphores;

	// NOTE: when adding new maps: also add mutex initializer in CreateDevice
};

struct Queue : Handle {
	Device* dev;

	VkQueue queue;
	VkQueueFlags flags;
	u32 family;
};

Renderer* getWindowRenderer(Device& dev);
Renderer* getOverlayRenderer(Swapchain& swapchain);

// Called by destructor of DeviceHandle.
// Expects device mutex to be locked.
void notifyDestruction(Device& dev, Handle& handle);

// Expects dev.mutex to be locked
template<typename F>
void forEachRendererLocked(Device& dev, F&& func) {
	for(auto& swapchain : dev.swapchains.map) {
		auto* renderer = getOverlayRenderer(*swapchain.second);
		if(renderer) {
			func(*renderer);
		}
	}

	auto* renderer = getWindowRenderer(dev);
	if(renderer) {
		func(*renderer);
	}
}

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
	VkPhysicalDevice phdev,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* alloc,
	VkDevice* dev);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
	VkDevice dev,
	VkAllocationCallbacks* alloc);

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(
	VkQueue                                     queue,
	uint32_t                                    submitCount,
	const VkSubmitInfo*                         pSubmits,
	VkFence                                     fence);

} // naemspace fuen
