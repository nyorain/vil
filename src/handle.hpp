#pragma once

#include <fwd.hpp>
#include <string>
#include <vector>
#include <atomic>
#include <vk/vulkan.h>

namespace vil {

// In-layer representation of a vulkan API object created by the application.
struct Handle {
	std::string name;

	Handle() = default;
	Handle(Handle&&) = delete;
	Handle& operator=(Handle&&) = delete;
};

struct SharedDeviceHandle : Handle {
	Device* dev {};
	std::atomic<u32> refCount {};

protected:
	// to prevent anyone from doing IntrusivePtr<SharedDeviceHandle>
	~SharedDeviceHandle() = default;
};

const char* name(VkObjectType objectType);
std::string name(const Handle& handle,
		VkObjectType objectType,
		bool addType = true,
		bool perTypeDefault = true);

template<typename T>
std::string name(const T& handle,
		bool addType = true,
		bool perTypeDefault = true) {
	return name(handle, handle.objectType, addType, perTypeDefault);
}

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
	virtual void visit(ComputePipeline& p);
	virtual void visit(GraphicsPipeline& p);
	virtual void visit(RayTracingPipeline& p);
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
	void visit(GraphicsPipeline& res) override { impl(res); }
	void visit(ComputePipeline& res) override { impl(res); }
	void visit(RayTracingPipeline& res) override { impl(res); }
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
	static const ObjectTypeHandler* handler(VkObjectType);

	virtual ~ObjectTypeHandler() = default;
	virtual VkObjectType objectType() const = 0;

	// Expects handle.objectType == this->objectType()
	virtual void visit(ResourceVisitor& visitor, Handle& handle) const = 0;

	// The following functions may use the device maps directly and
	// can expect the device mutex to be locked.
	// NOTE: not implemented for the DescriptorSet ObjectTypeHandler
	virtual std::vector<Handle*> resources(Device& dev, std::string_view search) const = 0;

	// Expects device mutex to be locked.
	// NOTE: even though this is called 'find' expects 'handleToFind' to
	// be valid, i.e. can't detect bogus ids.
	// TODO: rename to 'get' or something and return a reference.
	// NOTE: for DescriptorSet handles, must only be called while
	// API guarantees that the handle stays valid since its lifetime is not bound
	// to the device mutex.
	virtual Handle* find(Device& dev, u64 handleToFind, u64& fwdID) const = 0;
};

// api
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectNameInfoEXT*        pNameInfo);

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectTagInfoEXT*         pTagInfo);

} // namespace vil
