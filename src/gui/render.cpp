#include <gui/render.hpp>
#include <gui/commandHook.hpp>
#include <util/util.hpp>
#include <device.hpp>
#include <queue.hpp>
#include <imgui/imgui.h>

namespace vil {

// Draw
void swap(Draw& a, Draw& b) noexcept {
	using std::swap;
	swap(a.cb, b.cb);
	swap(a.dev, b.dev);
	swap(a.dsSelected, b.dsSelected);
	swap(a.fence, b.fence);
	swap(a.inUse, b.inUse);
	swap(a.indexBuffer, b.indexBuffer);
	swap(a.vertexBuffer, b.vertexBuffer);
	swap(a.readback.copy, b.readback.copy);
	swap(a.presentSemaphore, b.presentSemaphore);
	swap(a.futureSemaphore, b.futureSemaphore);
	swap(a.futureSemaphoreValue, b.futureSemaphoreValue);
	swap(a.usedHandles, b.usedHandles);
	swap(a.usedHookState, b.usedHookState);
	swap(a.waitedUpon, b.waitedUpon);
}

Draw::Draw() = default;

Draw::Draw(Draw&& rhs) noexcept {
	swap(*this, rhs);
}

Draw& Draw::operator=(Draw rhs) noexcept {
	swap(*this, rhs);
	return *this;
}

void Draw::init(Device& dev, VkCommandPool commandPool) {
	this->dev = &dev;

	// init data
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 1u;
	cbai.commandPool = commandPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &cbai, &cb));
	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, cb);
	nameHandle(dev, this->cb, "Draw:cb");

	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateFence(dev.handle, &fci, nullptr, &fence));
	nameHandle(dev, this->fence, "Draw:fence");

	VkSemaphoreCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &presentSemaphore));
	nameHandle(dev, this->presentSemaphore, "Draw:presentSemaphore");

	VkSemaphoreTypeCreateInfo tsInfo {};
	if(dev.timelineSemaphores) {
		tsInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		tsInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		tsInfo.initialValue = 0u;
		sci.pNext = &tsInfo;
	}

	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &futureSemaphore));
	nameHandle(dev, this->futureSemaphore, "Draw:futureSemaphore");

	VkDescriptorSetAllocateInfo dsai {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = dev.dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &dev.renderData->dsLayout;
	VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dsai, &dsSelected));
	nameHandle(dev, this->dsSelected, "Draw:dsSelected");
}

Draw::~Draw() {
	if(!dev) {
		return;
	}

	if(fence) {
		if(inUse) {
			dev->dispatch.WaitForFences(dev->handle, 1, &fence, true, UINT64_MAX);
		}

		dev->dispatch.DestroyFence(dev->handle, fence, nullptr);
	}

	dev->dispatch.DestroySemaphore(dev->handle, presentSemaphore, nullptr);
	dev->dispatch.DestroySemaphore(dev->handle, futureSemaphore, nullptr);

	// We rely on the commandPool being freed to implicitly free this
	// command buffer. Since the gui owns the command pool this isn't
	// a problem.
	// if(cb) {
	// 	dev->dispatch.FreeCommandBuffers(dev->handle, commandPool, 1, &cb);
	// }

	if(dsSelected) {
		dev->dispatch.FreeDescriptorSets(dev->handle, dev->dsPool, 1, &dsSelected);
	}

	// NOTE: could return them to the device pool alternatively
	for(auto sem : waitedUpon) {
		dev->dispatch.DestroySemaphore(dev->handle, sem, nullptr);
	}
}

// RenderBuffer
void RenderBuffer::init(Device& dev, VkImage img, VkFormat format,
		VkExtent2D extent, VkRenderPass rp, VkImageView depthView) {
	this->dev = &dev;
	this->image = img;

	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.components = {};
	ivi.format = format;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.layerCount = 1;
	ivi.subresourceRange.levelCount = 1;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &view));
	nameHandle(dev, this->view, "RenderBuffer:view");

	dlg_assert(depthView);
	VkImageView atts[] = {view, depthView};

	VkFramebufferCreateInfo fbi {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.attachmentCount = 2u;
	fbi.pAttachments = atts;
	fbi.layers = 1u;
	fbi.width = extent.width;
	fbi.height = extent.height;
	fbi.renderPass = rp;
	VK_CHECK(dev.dispatch.CreateFramebuffer(dev.handle, &fbi, nullptr, &fb));
	nameHandle(dev, this->fb, "RenderBuffer:fb");
}

RenderBuffer::~RenderBuffer() {
	if(!dev) {
		return;
	}

	if(fb) {
		dev->dispatch.DestroyFramebuffer(dev->handle, fb, nullptr);
	}
	if(view) {
		dev->dispatch.DestroyImageView(dev->handle, view, nullptr);
	}
}

// RenderData
void RenderData::init(Device& dev) {
	// samplers
	VkSamplerCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.minLod = -1000;
	sci.maxLod = 1000;
	sci.maxAnisotropy = 1.0f;
	VK_CHECK(dev.dispatch.CreateSampler(dev.handle, &sci, nullptr, &nearestSampler));
	nameHandle(dev, this->nearestSampler, "RenderData:nearestSampler");

	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	VK_CHECK(dev.dispatch.CreateSampler(dev.handle, &sci, nullptr, &linearSampler));
	nameHandle(dev, this->linearSampler, "RenderData:linearSampler");

	// descriptor set layout
	VkDescriptorSetLayoutBinding binding {};
	binding.binding = 0u;
	binding.descriptorCount = 1u;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo dslci {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 1u;
	dslci.pBindings = &binding;
	VK_CHECK(dev.dispatch.CreateDescriptorSetLayout(dev.handle, &dslci, nullptr, &dsLayout));
	nameHandle(dev, this->dsLayout, "RenderData:dsLayout");

	// pipeline layout
	// We just allocate the full push constant range that all implementations
	// must support.
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pcrs[0].offset = 0;
	// PERF: perf most pipelines don't need this much. Could create multiple
	// pipe layouts.
	pcrs[0].size = 128; // needed e.g. for vertex viewer pipeline

	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr, &pipeLayout));
	nameHandle(dev, this->pipeLayout, "RenderData:pipeLayout");
}

void RenderData::free(Device& dev) {
	if(!nearestSampler) {
		return;
	}

	dev.dispatch.DestroySampler(dev.handle, nearestSampler, nullptr);
	dev.dispatch.DestroySampler(dev.handle, linearSampler, nullptr);
	dev.dispatch.DestroyDescriptorSetLayout(dev.handle, dsLayout, nullptr);
	dev.dispatch.DestroyPipelineLayout(dev.handle, pipeLayout, nullptr);
	*this = {};
}

} // namespace vil
