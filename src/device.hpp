#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <util/syncedMap.hpp>
#include <util/span.hpp>

#include <vk/vulkan.h>
#include <vk/vk_layer.h>
#include <vk/dispatch_table.h>
#include <vk/object_types.h>

#include <shared_mutex>
#include <memory>
#include <atomic>
#include <optional>
#include <unordered_map>

namespace fuen {

struct Device {
	Instance* ini {};
	VkDevice handle {};
	VkPhysicalDevice phdev;
	VkLayerDispatchTable dispatch;

	std::vector<std::string> extensions;

	VkPhysicalDeviceProperties props {};
	VkPhysicalDeviceMemoryProperties memProps {};
	VkPhysicalDeviceFeatures enabledFeatures {};

	bool timelineSemaphores {}; // whether we have timeline smeaphores
	u64 maxTimelineSemaphoreValueDifference {};

	// Aside from properties, only the families used by device
	// are initialized.
	std::vector<QueueFamily> queueFamilies;

	PFN_vkSetDeviceLoaderData setDeviceLoaderData;

	// Vector of all queues.
	// Might include queues created by us.
	std::vector<std::unique_ptr<Queue>> queues;
	// A vector of all queue family indices for which a queue exists.
	// Needed for concurrent resources.
	// We additionally create a (resettable, primary) command pool
	// for every used queue index (access them via Queue::commandPool, only
	// managed here so we don't have multiple pools per family index).
	std::vector<u32> usedQueueFamilyIndices;

	// The queue we use for graphics submissions. Can be assumed to
	// be non-null.
	Queue* gfxQueue {};

	u32 hostVisibleMemTypeBits {};
	u32 deviceLocalMemTypeBits {};

	VkDescriptorPool dsPool {};
	std::unique_ptr<RenderData> renderData;

	std::unique_ptr<DisplayWindow> window;

	// The currently active gui. Might be null. There is never more than
	// one gui associated with a device.
	Gui* gui {};

	std::vector<VkFence> fencePool; // currently unused fences

	std::vector<VkSemaphore> semaphorePool; // currently used semaphores
	std::vector<VkSemaphore> resetSemaphores; // list of semaphores that are waiting to be reset

	std::vector<std::unique_ptr<PendingSubmission>> pending;

	// Mutex for general shared access.
	// While this mutex is locked, resources won't be inserted or
	// erased from the resource tables below (and therefore can't
	// logically be created or destroyed). Also used to synchronize
	// shared access to most resources (that can be mutated).
	std::shared_mutex mutex;

	// Mutex that is locked *while* doing a submission. The general mutex
	// won't be locked for that time. So when we want to do submissions
	// ourselves from a different thread on from within another call,
	// we have to lock this to make sure our submissions don't interfer
	// with application submissions (as well as with our own).
	// Note that in vulkan submission synchronization happens on per-device,
	// *not* on per-queue basis.
	std::mutex queueMutex;

	// NOTE: hacky as hell but can't work around it. Needed only by the
	// public API to communicate with the application.
	// Must only be accessed while the mutex is locked.
	// TODO: could keep a stack of swapchains to support the case
	// "create1; create2; destroy2; getLastCreated" (correctly returning 1).
	Swapchain* lastCreatedSwapchain {};

	SyncedUniqueUnorderedMap<VkSwapchainKHR, Swapchain> swapchains;
	SyncedUniqueUnorderedMap<VkImage, Image> images;
	SyncedUniqueUnorderedMap<VkImageView, ImageView> imageViews;
	SyncedUniqueUnorderedMap<VkSampler, Sampler> samplers;
	SyncedUniqueUnorderedMap<VkBuffer, Buffer> buffers;
	SyncedUniqueUnorderedMap<VkPipeline, ComputePipeline> computePipes;
	SyncedUniqueUnorderedMap<VkPipeline, GraphicsPipeline> graphicsPipes;
	SyncedUniqueUnorderedMap<VkFramebuffer, Framebuffer> framebuffers;
	SyncedUniqueUnorderedMap<VkRenderPass, RenderPass> renderPasses;
	SyncedUniqueUnorderedMap<VkCommandPool, CommandPool> commandPools;
	SyncedUniqueUnorderedMap<VkCommandBuffer, CommandBuffer> commandBuffers;
	SyncedUniqueUnorderedMap<VkFence, Fence> fences;
	SyncedUniqueUnorderedMap<VkDescriptorPool, DescriptorPool> dsPools;
	SyncedUniqueUnorderedMap<VkDescriptorSet, DescriptorSet> descriptorSets;
	SyncedUniqueUnorderedMap<VkShaderModule, ShaderModule> shaderModules;
	SyncedUniqueUnorderedMap<VkDeviceMemory, DeviceMemory> deviceMemories;
	SyncedUniqueUnorderedMap<VkEvent, Event> events;
	SyncedUniqueUnorderedMap<VkSemaphore, Semaphore> semaphores;
	SyncedUniqueUnorderedMap<VkQueryPool, QueryPool> queryPools;
	SyncedUniqueUnorderedMap<VkBufferView, BufferView> bufferViews;
	SyncedUniqueUnorderedMap<VkDescriptorUpdateTemplate, DescriptorUpdateTemplate> dsuTemplates;

	// Some of our handles have shared ownership: this is only used when
	// an application is allowed to destroy a handle that we might still
	// need in the future (if we only need its data, do it like its done
	// with RenderPass and just give the data inside the handle shared
	// ownership, not the handle itself). Mostly done for layouts, we can
	// expect the handles to be cheap to be kept alive anyways.

	// Descriptors allocated from the layout expect it to remain
	// valid. We might also (in future) create new descriptors from the
	// layout ourselves (e.g. for submission modification).
	SyncedIntrusiveUnorderedMap<VkDescriptorSetLayout, DescriptorSetLayout> dsLayouts;
	// An application can destroy a pipeline layout after a command
	// buffer is recorded without it becoming invalid. But we still need
	// the handle for internal hooked-recording.
	SyncedIntrusiveUnorderedMap<VkPipelineLayout, PipelineLayout> pipeLayouts;

	// NOTE: when adding new maps: also add mutex initializer in CreateDevice

	~Device();
};

Gui* getWindowGui(Device& dev);
Gui* getOverlayGui(Swapchain& swapchain);

// Does not expect mutex to be locked
void notifyDestruction(Device& dev, Handle& handle);

// Util for naming internal handles.
// Mainly useful to get better validation layer output for stuff
// we do inside the layer. Should never be used on any non-internal
// handles.
// TODO: does not seem to work inside layer, investigate. Likely
//   vulkan loader limitation.
void nameHandle(Device& dev, VkObjectType objType, u64 handle, const char* name);

template<typename VkT>
void nameHandle(Device& dev, VkT handle, const char* name) {
	auto objType = VkHandleInfo<VkT>::kVkObjectType;
	nameHandle(dev, objType, handleToU64(handle), name);
}

class DebugLabel {
public:
	DebugLabel(Device& dev, VkCommandBuffer, const char* name,
		std::array<float, 4> color = {});
	~DebugLabel();

protected:
	VkCommandBuffer cb_ {};
	const Device* dev_ {};
};

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
	VkPhysicalDevice phdev,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* alloc,
	VkDevice* dev);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
	VkDevice dev,
	const VkAllocationCallbacks* alloc);

} // naemspace fuen
