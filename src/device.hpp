#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <util/handleCast.hpp>
#include <util/syncedMap.hpp>
#include <util/debugMutex.hpp>
#include <util/profiling.hpp>
#include <nytl/span.hpp>

#include <vk/vulkan.h>
#include <vk/vk_layer.h>
#include <vk/dispatch_table.h>
#include <vk/object_types.h>

#include <set>
#include <shared_mutex>
#include <memory>
#include <atomic>
#include <optional>
#include <unordered_map>

#if VK_USE_64_BIT_PTR_DEFINES == 0
	// NOTE: 32-bit platforms cause multiple problems at the moment.
	// For instance, since vulkan handles aren't typesafe, we couldn't
	// build HandleDesc (and the utility surrounding it) the way we do.
	// Create an issue/pull request on github if you, dear reader, really need this.
	#error "vil does not support 32-bit platforms at the moment"
#endif // VK_USE_64_BIT_PTR_DEFINES

namespace vil {

template<typename T, std::size_t maxSize>
struct KeepAliveRingBuffer {
	std::vector<IntrusivePtr<T>> data;
	u64 insertOffset {};

	VIL_DEBUG_ONLY(
		using Clock = std::chrono::steady_clock;
		Clock::time_point lastWrap {};
	)

	// defined in wrap.hpp
	IntrusivePtr<T> pushLocked(IntrusivePtr<T> obj);
	void clear();
};

template<typename T>
struct UnreachableDeleter {
	void operator()(T* ptr) noexcept {
		if(ptr) {
			dlg_error("unreachable!");
		}
	}
};

template<typename T>
using IntrusiveDerivedPtr = IntrusivePtr<T, UnreachableDeleter<T>>;

template<typename T>
struct HandlePtrFactory<IntrusiveDerivedPtr<T>> {
	template<typename... Args>
	static IntrusiveDerivedPtr<T> create(Args&&...) {
		dlg_error("unreachable");
	}
};

template<typename K, typename T>
using SyncedIntrusiveDerivedUnorderedMap = SyncedUnorderedMap<K, T, IntrusiveDerivedPtr>;

struct DeviceAddressMap;

struct Device {
	Instance* ini {};
	VkDevice handle {};
	VkPhysicalDevice phdev;
	VkLayerDispatchTable dispatch;

	std::vector<std::string> appExts; // only extensions enabled by application
	std::vector<std::string> allExts; // all extensions; also the ones enabled by us

	VkPhysicalDeviceProperties props {};
	VkPhysicalDeviceMemoryProperties memProps {};

	// features enabled by application
	VkPhysicalDeviceFeatures enabledFeatures {};
	VkPhysicalDeviceVulkan11Features enabledFeatures11 {};
	VkPhysicalDeviceVulkan12Features enabledFeatures12 {};
	VkPhysicalDeviceVulkan13Features enabledFeatures13 {};
	// TODO: extension features. Maybe just build own ext chain here?

	// supported features/extensions
	bool timelineSemaphores {}; // whether we have timeline semaphores
	bool transformFeedback {}; // whether we have transformFeedback
	bool nonSolidFill {}; // whether we have nonSolidFill mode
	bool bufferDeviceAddress {}; // whether we have bufferDeviceAddress
	bool shaderStorageImageWriteWithoutFormat {};
	bool extDeviceFault {}; // whether EXT_device_fault was enabled

	// Only valid when EXT_device_address_binding_report enabled.
	std::unique_ptr<DeviceAddressMap> addressMap;

	// Whether we are in integration testing mode
	bool testing {};
	std::atomic<bool> doFullSync {};
	std::atomic<bool> captureCmdStack {};

	// Aside from properties, only the families used by device
	// are initialized.
	std::vector<QueueFamily> queueFamilies;

	PFN_vkSetDeviceLoaderData setDeviceLoaderData {};

	// Vector of all queues.
	// Might include queues created by us.
	std::vector<std::unique_ptr<Queue>> queues;
	// A vector of all queue family indices for which a queue exists.
	// Needed for concurrent resources.
	// We additionally create a (resettable, primary) command pool
	// for every used queue index (access them via Queue::commandPool, only
	// managed here so we don't have multiple pools per family index).
	std::vector<u32> usedQueueFamilyIndices;
	// Global submission counter - counts for all queues.
	// Is increased for every VkQueueSubmit call.
	std::atomic<u64> submissionCounter {0u};

	// The queue we use for graphics submissions. Can be assumed to
	// be non-null.
	Queue* gfxQueue {};

	u32 hostVisibleMemTypeBits {};
	u32 deviceLocalMemTypeBits {};

	// own static rendering stuff
	VkDescriptorPool dsPool {};
	VkSampler linearSampler {};
	VkSampler nearestSampler {};

	std::unique_ptr<DisplayWindow> window;

	// Always valid, initialized on device creation.
	std::unique_ptr<CommandHook> commandHook {};

	std::vector<VkFence> fencePool; // currently unused fences

	std::vector<VkSemaphore> semaphorePool; // currently used semaphores
	std::vector<VkSemaphore> resetSemaphores; // list of semaphores that are waiting to be reset

	// TODO: move to individual queues?
	std::vector<std::unique_ptr<SubmissionBatch>> pending;

	// Mutex for general shared access.
	// While this mutex is locked, resources won't be inserted or
	// erased from the resource tables below (and therefore can't
	// logically be created or destroyed). Also used to synchronize
	// shared access to most resources (that can be mutated).
	// vilDefSharedMutex(mutex);
	TracySharedLockable(DebugSharedMutex, mutex);

	// Mutex that is locked *while* doing a submission. The general mutex
	// won't be locked for that time. So when we want to do submissions
	// ourselves from a different thread on from within another call,
	// we have to lock this to make sure our submissions don't interfer
	// with application submissions (as well as with our own).
	// Note that in vulkan submission synchronization happens on per-device,
	// *not* on per-queue basis.
	vilDefMutex(queueMutex);

	// === VkBufferAddress lookup ===
	// In various places we need the buffer belonging to a given buffer address.
	// This data structure allows efficient insert, deletion and lookup.
	// Must be synchronized via the device mutex, prefer the utility function
	// in buffer.hpp.
	struct BufferAddressEntry {
		VkDeviceAddress address;
		Buffer* buffer;
	};

	struct BufferAddressCmp {
		using is_transparent = struct UghWtf {}; // See cppreference is_transparent
		bool operator()(const Buffer* a, const Buffer* b) const;
		bool operator()(VkDeviceAddress a, const Buffer* b) const;
		bool operator()(const Buffer* a, VkDeviceAddress b) const;
	};

	std::set<Buffer*, BufferAddressCmp> bufferAddresses;

	// === VkAccelerationStructureKHR lookup ===
	// When building top-level acceleration structured on the device, we
	// need to retrieve the acceleration structure for a given VkDeviceAddress.
	// This map allows it.
	// Access must be synchronized via the device mutex, prefer the utility
	// function in accelStruct.hpp.
	std::unordered_map<VkDeviceAddress, AccelStruct*> accelStructAddresses;

	// === Maps of all vulkan handles ===
	SyncedRawUnorderedMap<VkDescriptorSet, DescriptorSet> descriptorSets;
	SyncedIntrusiveWrappedUnorderedMap<VkCommandBuffer, CommandBuffer> commandBuffers;

	// Some of our handles have shared ownership: this is only used when
	// an application is allowed to destroy a handle that we might still
	// need in the future (if we only need its data, do it like its done
	// with RenderPass and just give the data inside the handle shared
	// ownership, not the handle itself). Mostly done for layouts, we can
	// expect the handles to be cheap to be kept alive anyways.

	// TODO: make them unordered set when we switch to c++20 and
	// have transparent lookup
	SyncedIntrusiveUnorderedMap<VkShaderModule, ShaderModule> shaderModules;
	SyncedIntrusiveUnorderedMap<VkSwapchainKHR, Swapchain> swapchains;
	SyncedIntrusiveUnorderedMap<VkImage, Image> images;
	SyncedIntrusiveUnorderedMap<VkFramebuffer, Framebuffer> framebuffers;
	SyncedIntrusiveUnorderedMap<VkCommandPool, CommandPool> commandPools;
	SyncedIntrusiveUnorderedMap<VkFence, Fence> fences;
	SyncedIntrusiveUnorderedMap<VkDescriptorPool, DescriptorPool> dsPools;
	SyncedIntrusiveUnorderedMap<VkDeviceMemory, DeviceMemory> deviceMemories;
	SyncedIntrusiveUnorderedMap<VkEvent, Event> events;
	SyncedIntrusiveUnorderedMap<VkSemaphore, Semaphore> semaphores;
	SyncedIntrusiveUnorderedMap<VkQueryPool, QueryPool> queryPools;
	SyncedIntrusiveUnorderedMap<VkDescriptorSetLayout, DescriptorSetLayout> dsLayouts;
	SyncedIntrusiveUnorderedMap<VkPipelineLayout, PipelineLayout> pipeLayouts;
	SyncedIntrusiveUnorderedMap<VkDescriptorUpdateTemplate, DescriptorUpdateTemplate> dsuTemplates;
	SyncedIntrusiveUnorderedMap<VkRenderPass, RenderPass> renderPasses;

	SyncedIntrusiveUnorderedSet<ImageView> imageViews;
	SyncedIntrusiveUnorderedSet<Sampler> samplers;
	SyncedIntrusiveUnorderedSet<Buffer> buffers;
	SyncedIntrusiveUnorderedSet<BufferView> bufferViews;
	SyncedIntrusiveUnorderedSet<AccelStruct> accelStructs;

	// NOTE: Even though we just store IntrusivePtr<Pipeline> here, the real
	// type is GraphicsPipeline, ComputePipeline or RayTracingPipeline.
	// When erasing from the map, mustMove should be used and the pipeline
	// casted since the destructor is not virtual.
	SyncedIntrusiveDerivedUnorderedMap<VkPipeline, Pipeline> pipes;

	// NOTE: when adding new maps: also add mutex initializer in CreateDevice

	// NOTE: keepAliveCount = 0 means it's disabled completely.
	// TODO: documentation on keep alive.
	// Note how we must only have the KeepAlive mechanism for handles
	// we wrap and for which we have an unordered set instead of
	// an unordered map.
	static constexpr auto keepAliveCount = 0u;

	KeepAliveRingBuffer<ImageView, keepAliveCount> keepAliveImageViews;
	KeepAliveRingBuffer<Sampler, keepAliveCount> keepAliveSamplers;
	KeepAliveRingBuffer<Buffer, keepAliveCount> keepAliveBuffers;
	KeepAliveRingBuffer<BufferView, keepAliveCount> keepAliveBufferViews;
	KeepAliveRingBuffer<AccelStruct, keepAliveCount> keepAliveAccelStructs;

public:
	Device(); // = default in src
	~Device();

	IntrusivePtr<Swapchain> swapchain();
	IntrusivePtr<Swapchain> swapchainPtrLocked();

	Swapchain* swapchainLocked() {
		assertOwned(this->mutex);
		return swapchain_.get();
	}

	// NOTE: returning this outside the critical section isn't a race,
	// we never destroy the Gui object while the device is alive.
	Gui* gui() {
		std::lock_guard lock(this->mutex);
		return gui_.get();
	}

	Gui* guiLocked() {
		assertOwned(this->mutex);
		return gui_.get();
	}

	Gui& getOrCreateGui(VkFormat colorFormat);
	void swapchain(IntrusivePtr<Swapchain>);
	void swapchainDestroyed(const Swapchain& swapchain);

private:
	// The currently active gui. Might be null. There is never more than
	// one gui associated with a device.
	// Synced via device mutex.
	std::unique_ptr<Gui> gui_ {};

	// The last created swapchain.
	// Synced via device mutex.
	// NOTE: hacky as hell but can't work around it. Needed only by the
	// public API to communicate with the application.
	// Must only be accessed while the mutex is locked.
	// NOTE: we don't really suport applications with multiple swapchains yet
	// TODO: could keep a stack of swapchains to support the case
	// "create1; create2; destroy2; getLastCreated" (correctly returning 1).
	IntrusivePtr<Swapchain> swapchain_ {};

};

// Expects device mutex to be locked.
void notifyApiHandleDestroyedLocked(Device& dev, Handle& handle, VkObjectType type);
void notifyMemoryResourceInvalidatedLocked(Device& dev, MemoryResource& res);

// Lazily initializes the window, if needed.
void checkInitWindow(Device& dev);

// Called when we detected VK_ERROR_DEVICE_LOST.
// Might be called while dev mutex is locked.
void onDeviceLost(Device& dev);

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

bool supportedUsage(VkFormatFeatureFlags features, VkImageUsageFlags usages, bool has11);
bool supported(Device& dev, const VkImageCreateInfo& info, VkFormatFeatureFlags additional);
VkFormat findSupported(const Device& dev, span<const VkFormat> formats,
		const VkImageCreateInfo& info, VkFormatFeatureFlags additional);
VkFormat findDepthFormat(const Device& dev);

bool hasAppExt(Device& dev, const char* extName);

struct StandaloneDeviceInfo {
	PFN_vkGetInstanceProcAddr getInstanceProcAddr;
	PFN_vkGetDeviceProcAddr getDeviceProcAddr;
};

// api
VkResult doCreateDevice(
	VkPhysicalDevice phdev,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* alloc,
	VkDevice* dev,
	StandaloneDeviceInfo* standalone = nullptr);

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
	VkPhysicalDevice phdev,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* alloc,
	VkDevice* dev);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
	VkDevice dev,
	const VkAllocationCallbacks* alloc);

} // naemspace vil
