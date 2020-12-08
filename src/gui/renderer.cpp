#include "renderer.hpp"
#include "util.hpp"
#include <imgui/imgui.h>

namespace fuen {

// Draw
void Draw::Buffer::ensure(Device& dev, VkDeviceSize reqSize,
		VkBufferUsageFlags usage) {
	if(size >= reqSize) {
		return;
	}

	// over-allocate
	reqSize *= 2;

	if(buf) {
		dev.dispatch.DestroyBuffer(dev.handle, buf, nullptr);
		dev.dispatch.FreeMemory(dev.handle, mem, nullptr);
	}

	// new buffer
	VkBufferCreateInfo bufInfo {};
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size = reqSize;
	bufInfo.usage = usage;
	VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bufInfo, nullptr, &buf));
	nameHandle(dev, this->buf, "Draw:Buffer:buf");

	// get memory props
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &mem));
	nameHandle(dev, this->mem, "Draw:Buffer:mem");

	// bind
	VK_CHECK(dev.dispatch.BindBufferMemory(dev.handle, buf, mem, 0));
	this->size = reqSize;
}

void Draw::Buffer::free(Device& dev) {
	if(buf) {
		dev.dispatch.DestroyBuffer(dev.handle, buf, nullptr);
	}

	if(mem) {
		dev.dispatch.FreeMemory(dev.handle, mem, nullptr);
	}
}

// Draw
void Draw::init(Device& dev) {
	this->dev = &dev;

	// init data
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 1u;
	cbai.commandPool = dev.commandPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &cbai, &cb));
	nameHandle(dev, this->cb, "Draw:cb");

	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, cb);

	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateFence(dev.handle, &fci, nullptr, &fence));
	nameHandle(dev, this->fence, "Draw:fence");

	VkSemaphoreCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VK_CHECK(dev.dispatch.CreateSemaphore(dev.handle, &sci, nullptr, &semaphore));
	nameHandle(dev, this->semaphore, "Draw:semaphore");

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

	vertexBuffer.free(*dev);
	indexBuffer.free(*dev);

	if(fence) {
		if(inUse) {
			dev->dispatch.WaitForFences(dev->handle, 1, &fence, true, UINT64_MAX);
		}

		dev->dispatch.DestroyFence(dev->handle, fence, nullptr);
	}

	if(semaphore) {
		dev->dispatch.DestroySemaphore(dev->handle, semaphore, nullptr);
	}

	if(cb) {
		dev->dispatch.FreeCommandBuffers(dev->handle, dev->commandPool, 1, &cb);
	}

	if(dsSelected) {
		dev->dispatch.FreeDescriptorSets(dev->handle, dev->dsPool, 1, &dsSelected);
	}
}

// RenderBuffer
void RenderBuffer::init(Device& dev, VkImage img, VkFormat format,
		VkExtent2D extent, VkRenderPass rp) {
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

	VkFramebufferCreateInfo fbi {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.attachmentCount = 1u;
	fbi.pAttachments = &view;
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
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
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
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pcrs[0].offset = sizeof(float) * 0;
	pcrs[0].size = sizeof(float) * 4;

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

} // namespace fuen
