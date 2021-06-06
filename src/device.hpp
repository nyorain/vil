#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <util/syncedMap.hpp>
#include <util/span.hpp>
#include <util/debugMutex.hpp>
#include <util/profiling.hpp>

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

struct DebugStats {
	static DebugStats& get();

	std::atomic<u32> aliveRecords {};
	std::atomic<u32> aliveDescriptorStates {};
	std::atomic<u32> aliveDescriptorSets {};
	std::atomic<u32> aliveBuffers {};
	std::atomic<u32> aliveImagesViews {};
	std::atomic<u32> aliveHookRecords {};
	std::atomic<u32> aliveHookStates {};

	std::atomic<u32> threadContextMem {};
	std::atomic<u32> commandMem {};
	std::atomic<u32> descriptorStateMem {};
};

struct Device {
	Instance* ini {};
	VkDevice handle {};
	VkPhysicalDevice phdev;
	VkLayerDispatchTable dispatch;

	std::vector<std::string> appExts; // only extensions enabled by application
	std::vector<std::string> allExts; // all extensions; also the ones enabled by us

	VkPhysicalDeviceProperties props {};
	VkPhysicalDeviceMemoryProperties memProps {};
	VkPhysicalDeviceFeatures enabledFeatures {}; // features by application

	// supported features/extensions
	bool timelineSemaphores {}; // whether we have timeline smeaphores
	bool transformFeedback {}; // whether we have transformFeedback
	bool nonSolidFill {}; // whether we have nonSolidFill mode
	bool bufferDeviceAddress {}; // whether we have bufferDeviceAddress

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
	std::atomic<u64> submissionCounter;

	// The queue we use for graphics submissions. Can be assumed to
	// be non-null.
	Queue* gfxQueue {};

	u32 hostVisibleMemTypeBits {};
	u32 deviceLocalMemTypeBits {};

	VkDescriptorPool dsPool {};
	std::unique_ptr<RenderData> renderData;

	std::unique_ptr<DisplayWindow> window;

	// Always valid, initialized on device creation.
	std::unique_ptr<CommandHook> commandHook {};

	// The currently active gui. Might be null. There is never more than
	// one gui associated with a device.
	Gui* gui {};
	Swapchain* swapchain {};

	std::vector<VkFence> fencePool; // currently unused fences

	std::vector<VkSemaphore> semaphorePool; // currently used semaphores
	std::vector<VkSemaphore> resetSemaphores; // list of semaphores that are waiting to be reset

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

	// NOTE: hacky as hell but can't work around it. Needed only by the
	// public API to communicate with the application.
	// Must only be accessed while the mutex is locked.
	// TODO: could keep a stack of swapchains to support the case
	// "create1; create2; destroy2; getLastCreated" (correctly returning 1).
	Swapchain* lastCreatedSwapchain {};

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
		using is_transparent = struct UghWtf {};
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
	SyncedUniqueWrappedUnorderedMap<VkCommandBuffer, CommandBuffer> commandBuffers;

	SyncedUniqueUnorderedMap<VkSwapchainKHR, Swapchain> swapchains;
	SyncedUniqueUnorderedMap<VkImage, Image> images;
	SyncedUniqueUnorderedMap<VkFramebuffer, Framebuffer> framebuffers;
	SyncedUniqueUnorderedMap<VkRenderPass, RenderPass> renderPasses;
	SyncedUniqueUnorderedMap<VkCommandPool, CommandPool> commandPools;
	SyncedUniqueUnorderedMap<VkFence, Fence> fences;
	SyncedUniqueUnorderedMap<VkDescriptorPool, DescriptorPool> dsPools;
	SyncedUniqueUnorderedMap<VkDescriptorSet, DescriptorSet> descriptorSets;
	SyncedUniqueUnorderedMap<VkShaderModule, ShaderModule> shaderModules;
	SyncedUniqueUnorderedMap<VkDeviceMemory, DeviceMemory> deviceMemories;
	SyncedUniqueUnorderedMap<VkEvent, Event> events;
	SyncedUniqueUnorderedMap<VkSemaphore, Semaphore> semaphores;
	SyncedUniqueUnorderedMap<VkQueryPool, QueryPool> queryPools;

	// NOTE: Even though we just store unique_ptr<Pipeline> here, the real
	// type is GraphicsPipeline, ComputePipeline or RayTracingPipeline.
	// When erasing from the map, mustMove should be used and the pipeline
	// casted since the destructor is not virtual.
	SyncedUniqueUnorderedMap<VkPipeline, Pipeline> pipes;

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
	// Needs to be ref-counted only for PushDescriptorSetWithTemplateCmd
	SyncedIntrusiveUnorderedMap<VkDescriptorUpdateTemplate, DescriptorUpdateTemplate> dsuTemplates;

	// Resources stored in descriptors need shared ownership since so that
	// we don't have to track ds <-> resource links which would be a massive
	// bottleneck.
	SyncedIntrusiveUnorderedMap<VkImageView, ImageView> imageViews;
	SyncedIntrusiveUnorderedMap<VkSampler, Sampler> samplers;
	SyncedIntrusiveUnorderedMap<VkBuffer, Buffer> buffers;
	SyncedIntrusiveUnorderedMap<VkBufferView, BufferView> bufferViews;
	SyncedIntrusiveUnorderedMap<VkAccelerationStructureKHR, AccelStruct> accelStructs;

	// NOTE: when adding new maps: also add mutex initializer in CreateDevice

	~Device();
};

Gui* getWindowGui(Device& dev);
Gui* getOverlayGui(Swapchain& swapchain);

// Does not expect mutex to be locked
void notifyDestruction(Device& dev, Handle& handle);

static constexpr auto enableHandleWrapping = true;
template<typename H> struct HandleDesc;

#define DefHandleDesc(VkHandle, OurHandle, devMap, isDispatchable, wrapDefault) \
	template<> struct HandleDesc<VkHandle> { \
		using type = OurHandle; \
		static auto& map(Device& dev) { return dev.devMap; } \
		static constexpr bool dispatchable = isDispatchable; \
		static inline bool wrap = wrapDefault && enableHandleWrapping; \
	}

DefHandleDesc(VkImageView, ImageView, imageViews, false, true);
DefHandleDesc(VkBufferView, BufferView, bufferViews, false, true);
DefHandleDesc(VkBuffer, Buffer, buffers, false, true);
DefHandleDesc(VkSampler, Sampler, samplers, false, true);
DefHandleDesc(VkDescriptorSet, DescriptorSet, descriptorSets, false, true);
DefHandleDesc(VkDescriptorPool, DescriptorPool, dsPools, false, true);
DefHandleDesc(VkDescriptorSetLayout, DescriptorSetLayout, dsLayouts, false, true);
DefHandleDesc(VkDescriptorUpdateTemplate, DescriptorUpdateTemplate, dsuTemplates, false, true);
DefHandleDesc(VkCommandPool, CommandPool, commandPools, false, true);
DefHandleDesc(VkPipelineLayout, PipelineLayout, pipeLayouts, false, true);
DefHandleDesc(VkAccelerationStructureKHR, AccelStruct, accelStructs, false, true);

DefHandleDesc(VkCommandBuffer, CommandBuffer, commandBuffers, true, true);

// TODO: enable wrapping for these as well. Might need to implement some
// additional functions first, especially for image (e.g. GetSubresourceLayout)
DefHandleDesc(VkImage, Image, images, false, false);
DefHandleDesc(VkDeviceMemory, DeviceMemory, deviceMemories, false, false);
DefHandleDesc(VkQueryPool, QueryPool, queryPools, false, false);
DefHandleDesc(VkPipeline, Pipeline, pipes, false, false);

#undef DefHandleDesc

template<> struct HandleDesc<VkDevice> {
	using type = Device;
	static constexpr bool dispatchable = true;
	static inline bool wrap = false; // TODO: enable when fixed everywhere?
};

template<typename H> auto& unwrapWrapped(H handle) {
	static_assert(HandleDesc<H>::dispatchable);
	using T = typename HandleDesc<H>::type;
	auto u = std::uintptr_t(handleToU64(handle));
	return *reinterpret_cast<WrappedHandle<T>*>(u);
}

template<typename H> auto& unwrap(H handle) {
	using T = typename HandleDesc<H>::type;
	auto u = std::uintptr_t(handleToU64(handle));
	if constexpr(HandleDesc<H>::dispatchable) {
		return reinterpret_cast<WrappedHandle<T>*>(u)->obj();
	} else {
		return *reinterpret_cast<T*>(u);
	}
}

template<typename H, typename T>
H castDispatch(Device& dev, WrappedHandle<T>& wrapped) {
	static_assert(HandleDesc<H>::dispatchable);
	static_assert(std::is_same_v<typename HandleDesc<H>::type, T>);

	if(!HandleDesc<H>::wrap) {
		if constexpr(std::is_same_v<T, CommandBuffer>) {
			return wrapped.obj().handle();
		} else {
			return wrapped.obj().handle;
		}
	}

	std::memcpy(&wrapped.dispatch, reinterpret_cast<void*>(dev.handle), sizeof(wrapped.dispatch));
	return u64ToHandle<H>(reinterpret_cast<std::uintptr_t>(&wrapped));
}

template<typename H, typename T>
H castDispatch(T& obj) {
	static_assert(!HandleDesc<H>::dispatchable);
	static_assert(std::is_same_v<typename HandleDesc<H>::type, T>);

	if(!HandleDesc<H>::wrap) {
		return obj.handle;
	}

	return u64ToHandle<H>(reinterpret_cast<std::uintptr_t>(&obj));
}

inline Device& getDevice(VkDevice handle) {
	if(HandleDesc<VkDevice>::wrap) {
		return unwrap(handle);
	}

	return getData<Device>(handle);
}

template<typename H> using MapHandle = typename HandleDesc<H>::type;

template<typename H> MapHandle<H>& get(Device& dev, H handle) {
	if(HandleDesc<H>::wrap) {
		return unwrap(handle);
	}

	return HandleDesc<H>::map(dev).get(handle);
}

template<typename H> MapHandle<H>& get(VkDevice vkDev, H handle) {
	if(HandleDesc<H>::wrap) {
		return unwrap(handle);
	}

	auto& dev = getData<Device>(vkDev);
	return HandleDesc<H>::map(dev).get(handle);
}


template<typename H> auto getPtr(Device& dev, H handle) {
	using OurHandle = MapHandle<H>;
	if(HandleDesc<H>::wrap) {
		// TODO: I guess dispatchable handles should never be retrieved
		// like this in the first place but directly instead?
		// Change this here (and in the function below)
		// static_assert(!HandleDesc<H>::dispatchable);
		if constexpr(HandleDesc<H>::dispatchable) {
			auto u = std::uintptr_t(handleToU64(handle));
			auto* wrapped = reinterpret_cast<WrappedHandle<OurHandle*>>(u);
			return IntrusiveWrappedPtr<WrappedHandle<OurHandle>>(wrapped);
		} else {
			return IntrusivePtr<OurHandle>(&unwrap(handle));
		}
	}

	return HandleDesc<H>::map(dev).getPtr(handle);
}

template<typename H> auto getPtr(VkDevice vkDev, H handle) {
	using OurHandle = MapHandle<H>;
	if(HandleDesc<H>::wrap) {
		if constexpr(HandleDesc<H>::dispatchable) {
			auto u = std::uintptr_t(handleToU64(handle));
			auto* wrapped = reinterpret_cast<WrappedHandle<OurHandle*>>(u);
			return IntrusiveWrappedPtr<WrappedHandle<OurHandle>>(wrapped);
		} else {
			return IntrusivePtr<OurHandle>(&unwrap(handle));
		}
	}

	auto& dev = getDevice(vkDev);
	return HandleDesc<H>::map(dev).getPtr(handle);
}

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

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(
	VkPhysicalDevice phdev,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* alloc,
	VkDevice* dev);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(
	VkDevice dev,
	const VkAllocationCallbacks* alloc);

} // naemspace vil
