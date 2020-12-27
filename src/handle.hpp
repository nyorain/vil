#pragma once

#include <fwd.hpp>
#include <data.hpp> // for handleToU64
#include <dlg/dlg.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <unordered_set>

#include <vulkan/vulkan.h>

namespace fuen {

struct Handle {
	std::string name;
	// NOTE: should this rather be an unordered multimap?
	// not clear from spec whether setting a tag is intended
	// to replace an old one.
	std::unordered_map<u64, std::vector<std::byte>> tags;
	VkObjectType objectType {};

	Handle() = default;
	Handle(Handle&&) = delete;
	Handle& operator=(Handle&&) = delete;
};

struct DeviceHandle : Handle {
	Device* dev {};

	// A list of all command buffers recordings referencing this handle in their
	// current record state.
	// On destruction, the handle will inform all of them that they
	// are now in an invalid state.
	// NOTE: the dynamic memory allocations we do here could become a
	// serious performance problem. In that case replace it by per-cb
	// 2D-linked-list (linked grid), see node 1648
	std::unordered_set<CommandRecord*> refRecords;

	// Expects that neither the device mutex nor its own mutex is locked.
	~DeviceHandle();

	// Will inform all command buffers that use this handle that they
	// have been invalidated.
	void invalidateCbs();
	void invalidateCbsLocked();
};

// TODO: replace by utils from vk/object_types.h?
template<typename T>
constexpr const char* handleName() {
	if constexpr(std::is_same_v<T, Device>) return "Device";
	else if constexpr(std::is_same_v<T, Queue>) return "Queue";
	else if constexpr(std::is_same_v<T, Image>) return "Image";
	else if constexpr(std::is_same_v<T, ImageView>) return "ImageView";
	else if constexpr(std::is_same_v<T, Sampler>) return "Sampler";
	else if constexpr(std::is_same_v<T, Buffer>) return "Buffer";
	else if constexpr(std::is_same_v<T, CommandBuffer>) return "CommandBuffer";
	else if constexpr(std::is_same_v<T, CommandPool>) return "CommandPool";
	else if constexpr(std::is_same_v<T, DescriptorSet>) return "DescriptorSet";
	else if constexpr(std::is_same_v<T, DescriptorSetLayout>) return "DescriptorSetLayout";
	else if constexpr(std::is_same_v<T, DescriptorPool>) return "DescriptorPool";
	else if constexpr(std::is_same_v<T, PipelineLayout>) return "PipelineLayout";
	else if constexpr(std::is_same_v<T, GraphicsPipeline>) return "GraphicsPipeline";
	else if constexpr(std::is_same_v<T, ComputePipeline>) return "ComputePipeline";
	else if constexpr(std::is_same_v<T, Fence>) return "Fence";
	else if constexpr(std::is_same_v<T, Event>) return "Event";
	else if constexpr(std::is_same_v<T, Semaphore>) return "Semaphore";
	else if constexpr(std::is_same_v<T, RenderPass>) return "RenderPass";
	else if constexpr(std::is_same_v<T, Swapchain>) return "Swapchain";
	else if constexpr(std::is_same_v<T, Framebuffer>) return "Framebuffer";
	else if constexpr(std::is_same_v<T, DeviceMemory>) return "DeviceMemory";
	else if constexpr(std::is_same_v<T, ShaderModule>) return "ShaderModule";
	else if constexpr(std::is_same_v<T, Pipeline>) return "Pipeline";
	else if constexpr(std::is_same_v<T, QueryPool>) return "QueryPool";
	else if constexpr(std::is_same_v<T, BufferView>) return "Bufferview";
	else static_assert(!std::is_same_v<T, T>, "Invalid handle type");
}

// And so He spoke:
//  "Letteth us go into the Holy Lands of SFINAE /
//   Where Your Souls may findeth rest /
//   Since'o He - and only Him - in all his Glory /
//   Has forseen this as Our Home!"
template<typename T>
auto handle(const T& handle) -> decltype(handle.handle()) {
	return handle.handle();
}

template<typename T>
auto handle(const T& handle) -> decltype(handle.handle) {
	return handle.handle;
}

template<typename T>
std::string name(const T& handle) {
	constexpr auto hn = handleName<T>();

	std::string name;
	if(handle.name.empty()) {
		auto id = handleToU64(fuen::handle(handle));
		name = dlg::format("{} {}{}", hn, std::hex, id);
	} else {
		name = dlg::format("{} {}", hn, handle.name);
	}

	return name;
}

// api
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectNameInfoEXT*        pNameInfo);

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectTagInfoEXT*         pTagInfo);

} // namespace fuen
