#include <vkutil/handles.hpp>
#include <device.hpp>

namespace vil::vku {

// QueryPool
QueryPool::QueryPool(Device& dev, const VkQueryPoolCreateInfo& qpi) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qpi, nullptr, &handle_));
}

void QueryPool::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyQueryPool(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// fence
Fence::Fence(Device& dev, const VkFenceCreateInfo& fci) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateFence(dev.handle, &fci, nullptr, &handle_));
}

void Fence::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyFence(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// semaphore
Semaphore::Semaphore(Device& dev, const VkSemaphoreCreateInfo& ci) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &ci, nullptr, &handle_));
}

Semaphore::Semaphore(Device& dev, VkSemaphoreType type, u64 initValue) {
	VkSemaphoreCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkSemaphoreTypeCreateInfo tci {};
	if(type != VK_SEMAPHORE_TYPE_BINARY) {
		tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		tci.semaphoreType = type;
		tci.initialValue = initValue;
		sci.pNext = &tci;
	} else {
		dlg_assert(initValue == 0u);
	}

	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &handle_));
}

void Semaphore::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroySemaphore(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// DescriptorSetLayout
DescriptorSetLayout::DescriptorSetLayout(Device& dev,
		const VkDescriptorSetLayoutCreateInfo& ci, StringParam name) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateDescriptorSetLayout(dev.handle, &ci, nullptr, &handle_));

	if(!name.empty()) {
		nameHandle(*this, name);
	}
}

DescriptorSetLayout::DescriptorSetLayout(Device& dev,
		span<VkDescriptorSetLayoutBinding> bindings,
		StringParam name,
		span<const VkDescriptorBindingFlags> flags) {
	dev_ = &dev;

	unsigned int highestBinding = 0u;
	for(auto& binding : bindings) {
		auto& bid = binding.binding;
		if(bid == autoDescriptorBinding) {
			bid = highestBinding++;
		} else {
			highestBinding = std::max(highestBinding, bid + 1);
		}
	}

	VkDescriptorSetLayoutCreateInfo dslci {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = bindings.size();
	dslci.pBindings = bindings.data();

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo;
	if(!flags.empty()) {
		flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		flagsInfo.bindingCount = flags.size();
		flagsInfo.pBindingFlags = flags.data();
		dslci.pNext = &flagsInfo;
	}

	VK_CHECK(dev.dispatch.CreateDescriptorSetLayout(dev.handle, &dslci, nullptr, &handle_));

	if(!name.empty()) {
		nameHandle(*this, name);
	}
}

VkDescriptorSetLayoutBinding descriptorBinding(VkDescriptorType type,
		VkShaderStageFlags stages, const VkSampler* samplers,
		u32 count) {
	VkDescriptorSetLayoutBinding ret {};
	ret.binding = autoDescriptorBinding;
	ret.descriptorType = type;
	ret.descriptorCount = count;
	ret.stageFlags = stages;
	ret.pImmutableSamplers = samplers;
	return ret;
}

void DescriptorSetLayout::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyDescriptorSetLayout(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// DescriptorPool
DescriptorPool::DescriptorPool(Device& dev, const VkDescriptorPoolCreateInfo& ci) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateDescriptorPool(dev.handle, &ci, nullptr, &handle_));
}

void DescriptorPool::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyDescriptorPool(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// PipelineLayout
PipelineLayout::PipelineLayout(Device& dev, const VkPipelineLayoutCreateInfo& ci,
		StringParam name) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &ci, nullptr, &handle_));

	if(!name.empty()) {
		nameHandle(*this, name);
	}
}

PipelineLayout::PipelineLayout(Device& dev,
		span<const VkDescriptorSetLayout> layouts,
		span<const VkPushConstantRange> ranges, StringParam name) {
	dev_ = &dev;

	VkPipelineLayoutCreateInfo pci {};
	pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pci.pPushConstantRanges = ranges.data();
	pci.pushConstantRangeCount = ranges.size();
	pci.pSetLayouts = layouts.data();
	pci.setLayoutCount = layouts.size();
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &pci, nullptr, &handle_));

	if(!name.empty()) {
		nameHandle(*this, name);
	}
}

void PipelineLayout::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyPipelineLayout(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// Pipeline
Pipeline::Pipeline(Device& dev, const VkGraphicsPipelineCreateInfo& gpi,
		VkPipelineCache cache) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateGraphicsPipelines(dev.handle, cache, 1u,
		&gpi, nullptr, &handle_));
}

Pipeline::Pipeline(Device& dev, const VkComputePipelineCreateInfo& cpi,
		VkPipelineCache cache) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, cache, 1u,
		&cpi, nullptr, &handle_));
}

Pipeline::Pipeline(Device& dev, const VkRayTracingPipelineCreateInfoKHR& rtpi,
		VkPipelineCache cache) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateRayTracingPipelinesKHR(dev.handle, {}, cache, 1u,
		&rtpi, nullptr, &handle_));
}

void Pipeline::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyPipeline(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// ImageHandle
ImageHandle::ImageHandle(Device& dev, const VkImageCreateInfo& ici) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &handle_));
}

void ImageHandle::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyImage(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// ImageView
ImageView::ImageView(Device& dev, const VkImageViewCreateInfo& ici,
		StringParam name) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ici, nullptr, &handle_));

	if(!name.empty()) {
		nameHandle(*this, name);
	}
}

void ImageView::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyImageView(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// BufferHandle
BufferHandle::BufferHandle(Device& dev, const VkBufferCreateInfo& bci) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bci, nullptr, &handle_));
}

void BufferHandle::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyBuffer(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

// ShaderModule
ShaderModule::ShaderModule(Device& dev, const VkShaderModuleCreateInfo& sci) {
	dev_ = &dev;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &sci, nullptr, &handle_));
}

ShaderModule::ShaderModule(Device& dev, span<const u32> data) {
	dev_ = &dev;

	VkShaderModuleCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	sci.codeSize = 4 * data.size();
	sci.pCode = data.data();
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &sci, nullptr, &handle_));
}

void ShaderModule::destroy() noexcept {
	if(handle_) {
		dev_->dispatch.DestroyShaderModule(dev_->handle, handle_, nullptr);
		handle_ = {};
		dev_ = {};
	}
}

} // namespace vil::vku

