#pragma once

#include <fwd.hpp>
#include <device.hpp>
#include <data.hpp>
#include <util/dlg.hpp>

namespace vil {

static constexpr auto enableHandleWrapping = true;
template<typename H> struct HandleDesc;

using CommandBufferPtr = IntrusiveWrappedPtr<CommandBuffer>;

#define DefHandleDesc(VkHandle, OurHandle, devMap, isDispatchable, wrapDefault) \
	template<> struct HandleDesc<VkHandle> { \
		using type = OurHandle; \
		static inline auto& map(Device& dev) { return dev.devMap; } \
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
DefHandleDesc(VkImage, Image, images, false, true);
DefHandleDesc(VkPipeline, Pipeline, pipes, false, true);

DefHandleDesc(VkCommandBuffer, CommandBuffer, commandBuffers, true, true);
DefHandleDesc(VkEvent, Event, events, false, true);

// NOTE: to enable wrapping for those handles we might have to fix a couple
// of places inside the layer where they aren't correctly forwarded yet.
// But these handles aren't really used on performance-critical paths anyways
DefHandleDesc(VkDeviceMemory, DeviceMemory, deviceMemories, false, false);
DefHandleDesc(VkQueryPool, QueryPool, queryPools, false, false);
DefHandleDesc(VkRenderPass, RenderPass, renderPasses, false, false);
DefHandleDesc(VkFramebuffer, Framebuffer, framebuffers, false, false);
DefHandleDesc(VkFence, Fence, fences, false, false);
DefHandleDesc(VkSemaphore, Semaphore, semaphores, false, false);
DefHandleDesc(VkShaderModule, ShaderModule, shaderModules, false, false);
DefHandleDesc(VkSwapchainKHR, Swapchain, swapchains, false, false);

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
		// NOTE: kinda scetchy double reinterpret cast because T
		// might be incomplete
		auto& obj = reinterpret_cast<WrappedHandle<Handle>*>(u)->obj();
		return reinterpret_cast<T&>(obj);
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

inline ImageView& get(Device&, VkImageView handle) { return unwrap(handle); }
inline BufferView& get(Device&, VkBufferView handle) { return unwrap(handle); }
inline AccelStruct& get(Device&, VkAccelerationStructureKHR handle) { return unwrap(handle); }
inline Buffer& get(Device&, VkBuffer handle) { return unwrap(handle); }
inline Sampler& get(Device&, VkSampler handle) { return unwrap(handle); }

inline ImageView& getLocked(Device&, VkImageView handle) { return unwrap(handle); }
inline BufferView& getLocked(Device&, VkBufferView handle) { return unwrap(handle); }
inline AccelStruct& getLocked(Device&, VkAccelerationStructureKHR handle) { return unwrap(handle); }
inline Buffer& getLocked(Device&, VkBuffer handle) { return unwrap(handle); }
inline Sampler& getLocked(Device&, VkSampler handle) { return unwrap(handle); }

inline ImageView& get(VkDevice, VkImageView handle) { return unwrap(handle); }
inline BufferView& get(VkDevice, VkBufferView handle) { return unwrap(handle); }
inline AccelStruct& get(VkDevice, VkAccelerationStructureKHR handle) { return unwrap(handle); }
inline Buffer& get(VkDevice, VkBuffer handle) { return unwrap(handle); }
inline Sampler& get(VkDevice, VkSampler handle) { return unwrap(handle); }

template<typename = void>
inline IntrusivePtr<Sampler> getPtr(Device&, VkSampler handle) { return IntrusivePtr<Sampler>(&unwrap(handle)); }

template<typename H> MapHandle<H>& get(Device& dev, H handle) {
	if(HandleDesc<H>::wrap) {
		return unwrap(handle);
	}

	return HandleDesc<H>::map(dev).get(handle);
}

template<typename H> MapHandle<H>& getLocked(Device& dev, H handle) {
	if(HandleDesc<H>::wrap) {
		return unwrap(handle);
	}

	return HandleDesc<H>::map(dev).getLocked(handle);
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

// Removes the given handle from the associated map in the given device.
// Will additionally unset the internal forward handle in the given object.
template<typename H> auto mustMoveUnset(Device& dev, H& handle) {
	auto lock = std::lock_guard(dev.mutex);
	auto ptr = HandleDesc<H>::map(dev).mustMoveLocked(handle);
	handle = ptr->handle;
	ptr->handle = {};
	return ptr;
}

template<typename H> auto mustMoveUnset(VkDevice vkDev, H& handle) {
	if(HandleDesc<H>::wrap) {
		auto& h = unwrap(handle);
		auto& dev = *h.dev;
		return mustMoveUnset(dev, handle);
	} else {
		auto& dev = getDevice(vkDev);
		return mustMoveUnset(dev, handle);
	}
}

template<typename T, std::size_t maxSize>
void KeepAliveRingBuffer<T, maxSize>::push(T obj) {
	using H = decltype(obj->handle);

	if constexpr(maxSize == 0u) {
		std::unique_lock lock(obj->dev->mutex);
		auto ptr = HandleDesc<H>::map(*obj->dev).mustMoveLocked(*obj);
		ptr->handle = {}; // make sure to unset handle, marking it as destroyed
		// unlock before ptr gets destroyed and potentially destroys the object
		lock.unlock();
		return;
	} else {
		// keep alive to make sure we destroy it ouside of the cirtical section
		decltype(HandleDesc<H>::map(*obj->dev).mustMoveLocked(*obj)) ptr;
		std::lock_guard lock(*this->mutex);

		obj->handle = {};
		if(data.size() == maxSize) {
			auto& old = data[insertOffset];
			ptr = HandleDesc<H>::map(*obj->dev).mustMoveLocked(*old);

			VIL_DEBUG_ONLY(
				if(insertOffset == 0u) {
					auto now = Clock::now();
					auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastWrap);
					// The shorter this is, the higher the chance for false
					// positives (i.e. incorrect handles shown in gui) for
					// descriptorSets when running with refBindings = false
					dlg_warn("KeepAliveRingBuffer<{}> wrap took {}s",
						typeid(*obj).name(), dur.count());
					lastWrap = now;
				}
			)

			data[insertOffset] = std::move(obj);
			insertOffset = (insertOffset + 1) % maxSize;
		} else {
			VIL_DEBUG_ONLY(
				if(data.size() == 0u) {
					lastWrap = Clock::now();
				}
			)

			data.push_back(std::move(obj));
		}
	}
}

template<typename T, std::size_t maxSize>
void KeepAliveRingBuffer<T, maxSize>::clear() {
	for(auto* obj : data) {
		using H = decltype(obj->handle);
		HandleDesc<H>::map(*obj->dev).mustMove(*obj);
	}

	data.clear();
}

} // namespace vil
