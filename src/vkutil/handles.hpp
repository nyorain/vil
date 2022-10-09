/// File defines simple RAII wrappers around vulkan resources.
/// They usually don't have any additional features except more comfortable
/// construction and automatic resource destruction in the destructor.

#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>
#include <util/handleCast.hpp>
#include <nytl/span.hpp>
#include <nytl/stringParam.hpp>

namespace vil {

// From device.hpp.
void nameHandle(Device& dev, VkObjectType objType, u64 handle, const char* name);

} // namespace vil

namespace vil::vku {

template<typename Derived, class Handle, VkObjectType OT>
class Resource {
public:
	Device& dev() const noexcept { return *dev_; }
	const Handle& vkHandle() const noexcept { return handle_; }
	explicit operator const Handle&() const noexcept { return vkHandle(); }

	Handle release() {
		auto ret = handle_;
		handle_ = {};
		dev_ = {};
		return ret;
	}

	Resource() noexcept = default;
	Resource(Device& dev, const Handle& h) noexcept : dev_(&dev), handle_(h) {}
	~Resource() = default;

	Resource(Resource&& rhs) noexcept : dev_(rhs.dev_), handle_(rhs.handle_) {
		rhs.handle_ = {};
		rhs.dev_ = {};
	}

	Resource& operator=(Resource&& rhs) noexcept {
		static_cast<Derived&>(*this).destroy();
		handle_ = rhs.handle_;
		dev_ = rhs.dev_;
		rhs.handle_ = {};
		rhs.dev_ = {};
		return *this;
	}

	static VkObjectType vkObjectType() {
		return OT;
	}

protected:
	Device* dev_ {};
	Handle handle_ {};
};

class QueryPool : public Resource<QueryPool, VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL> {
public:
	QueryPool() = default;
	explicit QueryPool(Device& dev, VkQueryPool h) : Resource(dev, h) {}
	explicit QueryPool(Device& dev, const VkQueryPoolCreateInfo&);
	~QueryPool() { destroy(); }

	QueryPool(QueryPool&& rhs) noexcept = default;
	QueryPool& operator=(QueryPool&& rhs) noexcept = default;

	void destroy() noexcept;
};

class Fence : public Resource<Fence, VkFence, VK_OBJECT_TYPE_FENCE> {
public:
	Fence() = default;
	explicit Fence(Device& dev, VkFence h) : Resource(dev, h) {}
	explicit Fence(Device& dev); // default-constructs fence
	explicit Fence(Device&, const VkFenceCreateInfo&);
	~Fence() { destroy(); }

	Fence(Fence&& rhs) noexcept = default;
	Fence& operator=(Fence&& rhs) noexcept = default;

	void destroy() noexcept;
};

class Semaphore : public Resource<Semaphore, VkSemaphore, VK_OBJECT_TYPE_SEMAPHORE> {
public:
	Semaphore() = default;
	explicit Semaphore(Device& dev, VkSemaphore h) : Resource(dev, h) {}
	explicit Semaphore(Device& dev,
		VkSemaphoreType type = VK_SEMAPHORE_TYPE_BINARY,
		u64 initValue = 0u);
	explicit Semaphore(Device& dev, const VkSemaphoreCreateInfo&);
	~Semaphore() { destroy(); }

	Semaphore(Semaphore&& rhs) noexcept = default;
	Semaphore& operator=(Semaphore&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// Alternative vk::DescriptorSetLayoutBinding constructor.
/// When passed to the DescriptorSetLayout constructor, will automatically
/// update binding number without spaces if it is autoDescriptorBinding (-1).
constexpr auto autoDescriptorBinding = u32(0xFFFFFFFF);
VkDescriptorSetLayoutBinding descriptorBinding(VkDescriptorType type,
	VkShaderStageFlags stages = VK_SHADER_STAGE_ALL,
	const VkSampler* samplers = nullptr, u32 count = 1);

class DescriptorSetLayout : public Resource<DescriptorSetLayout, VkDescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT> {
public:
	DescriptorSetLayout() = default;
	explicit DescriptorSetLayout(Device& dev, VkDescriptorSetLayout h) : Resource(dev, h) {}
	explicit DescriptorSetLayout(Device& dev,
		const VkDescriptorSetLayoutCreateInfo&, StringParam name = {});

	/// Will modify the passed DescriptorSetLayoutBindings, i.e. automatically
	/// numerate them if they have binding == autoDescriptorBindings (-1),
	/// as is the default of vpp::descriptorBinding.
	/// This is only really useful if all bindings are using
	/// autoDescriptorBinding; for a descriptor set layout without holes.
	explicit DescriptorSetLayout(Device& dev,
		span<VkDescriptorSetLayoutBinding>,
		StringParam name = {},
		span<const VkDescriptorBindingFlags> flags = {});

	~DescriptorSetLayout() { destroy(); }

	DescriptorSetLayout(DescriptorSetLayout&& rhs) noexcept = default;
	DescriptorSetLayout& operator=(DescriptorSetLayout&& rhs) noexcept = default;

	void destroy() noexcept;
};

class DescriptorPool : public Resource<DescriptorPool, VkDescriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL> {
public:
	DescriptorPool() = default;
	explicit DescriptorPool(Device& dev, VkDescriptorPool h) : Resource(dev, h) {}
	explicit DescriptorPool(Device& dev, const VkDescriptorPoolCreateInfo&);
	~DescriptorPool() { destroy(); }

	DescriptorPool(DescriptorPool&& rhs) noexcept = default;
	DescriptorPool& operator=(DescriptorPool&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// RAII vulkan pipeline layout wrapper.
class PipelineLayout : public Resource<PipelineLayout, VkPipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT> {
public:
	PipelineLayout() = default;
	explicit PipelineLayout(Device& dev, VkPipelineLayout h) : Resource(dev, h) {}
	explicit PipelineLayout(Device& dev, const VkPipelineLayoutCreateInfo& info,
		StringParam name = {});
	explicit PipelineLayout(Device& dev,
		span<const VkDescriptorSetLayout> layouts,
		span<const VkPushConstantRange> ranges,
		StringParam name = {});
	~PipelineLayout() { destroy(); }

	PipelineLayout(PipelineLayout&& rhs) noexcept = default;
	PipelineLayout& operator=(PipelineLayout&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// RAII Vulkan pipeline wrapper.
class Pipeline : public Resource<Pipeline, VkPipeline, VK_OBJECT_TYPE_PIPELINE> {
public:
	Pipeline() = default;
	explicit Pipeline(Device& dev, VkPipeline pipe) : Resource(dev, pipe) {}
	explicit Pipeline(Device& dev, const VkGraphicsPipelineCreateInfo&,
		VkPipelineCache cache = {});
	explicit Pipeline(Device& dev, const VkComputePipelineCreateInfo&,
		VkPipelineCache cache = {});
	explicit Pipeline(Device& dev, const VkRayTracingPipelineCreateInfoKHR&,
		VkPipelineCache cache = {});
	~Pipeline() { destroy(); }

	Pipeline(Pipeline&& rhs) noexcept = default;
	Pipeline& operator=(Pipeline&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// RAII vk::Image wrapper.
/// Does not manage any memory, see vpp::Image.
class ImageHandle : public Resource<Image, VkImage, VK_OBJECT_TYPE_IMAGE> {
public:
	ImageHandle() = default;
	explicit ImageHandle(Device& dev, VkImage h) : Resource(dev, h) {}
	explicit ImageHandle(Device& dev, const VkImageCreateInfo&);
	~ImageHandle() { destroy(); }

	ImageHandle(ImageHandle&& rhs) noexcept = default;
	ImageHandle& operator=(ImageHandle&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// RAII wrapper around a vulkan image view.
class ImageView : public Resource<ImageView, VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW> {
public:
	ImageView() = default;
	ImageView(Device& dev, VkImageView h) : Resource(dev, h) {}
	ImageView(Device& dev, const VkImageViewCreateInfo&, StringParam name);
	~ImageView() { destroy(); }

	ImageView(ImageView&& rhs) noexcept = default;
	ImageView& operator=(ImageView&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// RAII vk::Buffer wrapper.
/// Does not manage any memory, see vpp::Buffer.
class BufferHandle : public Resource<BufferHandle, VkBuffer, VK_OBJECT_TYPE_BUFFER> {
public:
	BufferHandle() = default;
	explicit BufferHandle(Device& dev, VkBuffer h) : Resource(dev, h) {}
	explicit BufferHandle(Device& dev, const VkBufferCreateInfo&);
	~BufferHandle() { destroy(); }

	BufferHandle(BufferHandle&& rhs) noexcept = default;
	BufferHandle& operator=(BufferHandle&& rhs) noexcept = default;

	void destroy() noexcept;
};

/// RAII wrapper for a vulkan shader module.
class ShaderModule : public Resource<ShaderModule, VkShaderModule, VK_OBJECT_TYPE_SHADER_MODULE> {
public:
	ShaderModule() = default;
	explicit ShaderModule(Device& dev, VkShaderModule h) : Resource(dev, h) {}
	explicit ShaderModule(Device& dev, span<const u32> data);
	explicit ShaderModule(Device& dev, const VkShaderModuleCreateInfo&);
	~ShaderModule() { destroy(); }

	ShaderModule(ShaderModule&& rhs) noexcept = default;
	ShaderModule& operator=(ShaderModule&& rhs) noexcept = default;

	void destroy() noexcept;
};

template<typename T>
void nameHandle(const T& handle, StringParam name) {
	return nameHandle(handle.dev(), T::vkObjectType(),
		handleToU64(handle.vkHandle()), name.c_str());
}

} // namespace vil::vku

