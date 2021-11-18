#pragma once

#include <fwd.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <vk/vulkan.h>

namespace vil {

struct Handle {
	std::string name;
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
	UsedHandle* refRecords {};

	// Expects that neither the device mutex nor its own mutex is locked.
	~DeviceHandle();

	// Will inform all command buffers that use this handle that they
	// have been invalidated.
	void invalidateCbs();
	void invalidateCbsLocked();
};

const char* name(VkObjectType objectType);
std::string name(const Handle& handle);

struct ResourceVisitor {
	virtual ~ResourceVisitor() = default;
	virtual void visit(Image&) = 0;
	virtual void visit(ImageView&) = 0;
	virtual void visit(Sampler&) = 0;
	virtual void visit(Buffer&) = 0;
	virtual void visit(BufferView&) = 0;
	virtual void visit(CommandBuffer&) = 0;
	virtual void visit(CommandPool&) = 0;
	virtual void visit(DeviceMemory&) = 0;
	virtual void visit(Pipeline&) = 0;
	virtual void visit(PipelineLayout&) = 0;
	virtual void visit(DescriptorSet&) = 0;
	virtual void visit(DescriptorPool&) = 0;
	virtual void visit(DescriptorSetLayout&) = 0;
	virtual void visit(DescriptorUpdateTemplate&) = 0;
	virtual void visit(Framebuffer&) = 0;
	virtual void visit(RenderPass&) = 0;
	virtual void visit(QueryPool&) = 0;
	virtual void visit(Fence&) = 0;
	virtual void visit(Semaphore&) = 0;
	virtual void visit(Event&) = 0;
	virtual void visit(Queue&) = 0;
	virtual void visit(Swapchain&) = 0;
	virtual void visit(ShaderModule&) = 0;
	virtual void visit(AccelStruct&) = 0;
};

template<typename F>
struct TemplateResourceVisitor : ResourceVisitor {
	F impl;

	TemplateResourceVisitor(F&& f) : impl(std::move(f)) {}

	void visit(Image& res) override { impl(res); }
	void visit(ImageView& res) override { impl(res); }
	void visit(Sampler& res) override { impl(res); }
	void visit(Buffer& res) override { impl(res); }
	void visit(BufferView& res) override { impl(res); }
	void visit(CommandBuffer& res) override { impl(res); }
	void visit(CommandPool& res) override { impl(res); }
	void visit(DeviceMemory& res) override { impl(res); }
	void visit(Pipeline& res) override { impl(res); }
	void visit(PipelineLayout& res) override { impl(res); }
	void visit(DescriptorSet& res) override { impl(res); }
	void visit(DescriptorPool& res) override { impl(res); }
	void visit(DescriptorSetLayout& res) override { impl(res); }
	void visit(DescriptorUpdateTemplate& res) override { impl(res); }
	void visit(Framebuffer& res) override { impl(res); }
	void visit(RenderPass& res) override { impl(res); }
	void visit(QueryPool& res) override { impl(res); }
	void visit(Fence& res) override { impl(res); }
	void visit(Semaphore& res) override { impl(res); }
	void visit(Event& res) override { impl(res); }
	void visit(Queue& res) override { impl(res); }
	void visit(Swapchain& res) override { impl(res); }
	void visit(ShaderModule& res) override { impl(res); }
	void visit(AccelStruct& res) override { impl(res); }
};

struct ObjectTypeHandler {
	static const span<const ObjectTypeHandler*> handlers;

	virtual ~ObjectTypeHandler() = default;
	virtual VkObjectType objectType() const = 0;

	// Expects handle.objectType == this->objectType()
	virtual void visit(ResourceVisitor& visitor, Handle& handle) const = 0;

	// The following functions may use the device maps directly and
	// can expect the device mutex to be locked.
	virtual std::vector<Handle*> resources(Device& dev, std::string_view search) const = 0;
	virtual Handle* find(Device& dev, u64, u64& fwdID) const = 0;
};

template<typename T>
auto handle(const T& handle) -> decltype(handle.handle()) {
	return handle.handle();
}

template<typename T>
auto handle(const T& handle) -> decltype(handle.handle) {
	return handle.handle;
}

// api
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectNameInfoEXT*        pNameInfo);

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectTagInfoEXT*         pTagInfo);

} // namespace vil
