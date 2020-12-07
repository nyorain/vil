#include "renderer.hpp"
#include "util.hpp"
#include <imgui/imgui.h>

#include "overlay.frag.spv.h"
#include "overlay.vert.spv.h"

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
		dev.dispatch.vkDestroyBuffer(dev.handle, buf, nullptr);
		dev.dispatch.vkFreeMemory(dev.handle, mem, nullptr);
	}

	// new buffer
	VkBufferCreateInfo bufInfo = vk::BufferCreateInfo();
	bufInfo.size = reqSize;
	bufInfo.usage = usage;
	VK_CHECK(dev.dispatch.vkCreateBuffer(dev.handle, &bufInfo, nullptr, &buf));

	// get memory props
	VkMemoryRequirements memReqs;
	dev.dispatch.vkGetBufferMemoryRequirements(dev.handle, buf, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo = vk::MemoryAllocateInfo();
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.handle, &allocInfo, nullptr, &mem));

	// bind
	VK_CHECK(dev.dispatch.vkBindBufferMemory(dev.handle, buf, mem, 0));
	this->size = reqSize;
}

void Draw::Buffer::free(Device& dev) {
	if(buf) {
		dev.dispatch.vkDestroyBuffer(dev.handle, buf, nullptr);
	}

	if(mem) {
		dev.dispatch.vkFreeMemory(dev.handle, mem, nullptr);
	}
}

// Draw
void Draw::init(Device& dev) {
	// init data
	VkCommandBufferAllocateInfo cbai = vk::CommandBufferAllocateInfo();
	cbai.commandBufferCount = 1u;
	cbai.commandPool = dev.commandPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VK_CHECK(dev.dispatch.vkAllocateCommandBuffers(dev.handle, &cbai, &cb));

	// command buffer is a dispatchable object
	dev.setDeviceLoaderData(dev.handle, cb);

	VkFenceCreateInfo fci = vk::FenceCreateInfo();
	VK_CHECK(dev.dispatch.vkCreateFence(dev.handle, &fci, nullptr, &fence));

	VkSemaphoreCreateInfo sci = vk::SemaphoreCreateInfo();
	VK_CHECK(dev.dispatch.vkCreateSemaphore(dev.handle, &sci, nullptr, &semaphore));

	VkDescriptorSetAllocateInfo dsai = vk::DescriptorSetAllocateInfo();
	dsai.descriptorPool = dev.dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &dev.renderData->dsLayout;
	VK_CHECK(dev.dispatch.vkAllocateDescriptorSets(dev.handle, &dsai, &dsSelected));
}

Draw::~Draw() {
	if(!dev) {
		return;
	}

	vertexBuffer.free(*dev);
	indexBuffer.free(*dev);

	if(semaphore) {
		dev->dispatch.vkDestroySemaphore(dev->handle, semaphore, nullptr);
	}

	if(fence) {
		dev->dispatch.vkWaitForFences(dev->handle, 1, &fence, true, UINT64_MAX);
		dev->dispatch.vkDestroyFence(dev->handle, fence, nullptr);
	}

	if(cb) {
		dev->dispatch.vkFreeCommandBuffers(dev->handle, dev->commandPool, 1, &cb);
	}

	if(dsSelected) {
		dev->dispatch.vkFreeDescriptorSets(dev->handle, dev->dsPool, 1, &dsSelected);
	}
}

// RenderBuffer
void RenderBuffer::init(Device& dev, VkImage img, VkFormat format,
		VkExtent2D extent, VkRenderPass rp) {
	this->dev = &dev;
	this->image = img;

	VkImageViewCreateInfo ivi = vk::ImageViewCreateInfo();
	ivi.image = image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.components = {};
	ivi.format = format;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.layerCount = 1;
	ivi.subresourceRange.levelCount = 1;
	VK_CHECK(dev.dispatch.vkCreateImageView(dev.handle, &ivi, nullptr, &view));

	VkFramebufferCreateInfo fbi = vk::FramebufferCreateInfo();
	fbi.attachmentCount = 1u;
	fbi.pAttachments = &view;
	fbi.layers = 1u;
	fbi.width = extent.width;
	fbi.height = extent.height;
	fbi.renderPass = rp;
	VK_CHECK(dev.dispatch.vkCreateFramebuffer(dev.handle, &fbi, nullptr, &fb));
}

RenderBuffer::~RenderBuffer() {
	if(dev) {
		if(fb) {
			dev->dispatch.vkDestroyFramebuffer(dev->handle, fb, nullptr);
		}
		if(view) {
			dev->dispatch.vkDestroyImageView(dev->handle, view, nullptr);
		}
	}
}

// RenderData
void RenderData::init(Device& dev) {
	// samplers
	VkSamplerCreateInfo sci = vk::SamplerCreateInfo();
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.minLod = -1000;
	sci.maxLod = 1000;
	sci.maxAnisotropy = 1.0f;
	VK_CHECK(dev.dispatch.vkCreateSampler(dev.handle, &sci, nullptr, &nearestSampler));

	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	VK_CHECK(dev.dispatch.vkCreateSampler(dev.handle, &sci, nullptr, &linearSampler));

	// descriptor set layout
	VkDescriptorSetLayoutBinding binding;
	binding.binding = 0u;
	binding.descriptorCount = 1u;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo dslci = vk::DescriptorSetLayoutCreateInfo();
	dslci.bindingCount = 1u;
	dslci.pBindings = &binding;
	VK_CHECK(dev.dispatch.vkCreateDescriptorSetLayout(dev.handle, &dslci, nullptr, &dsLayout));

	// pipeline layout
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pcrs[0].offset = sizeof(float) * 0;
	pcrs[0].size = sizeof(float) * 4;

	VkPipelineLayoutCreateInfo plci = vk::PipelineLayoutCreateInfo();
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	VK_CHECK(dev.dispatch.vkCreatePipelineLayout(dev.handle, &plci, nullptr, &pipeLayout));
}

void RenderData::free(Device& dev) {
	dev.dispatch.vkDestroySampler(dev.handle, nearestSampler, nullptr);
	dev.dispatch.vkDestroySampler(dev.handle, linearSampler, nullptr);
	dev.dispatch.vkDestroyDescriptorSetLayout(dev.handle, dsLayout, nullptr);
	dev.dispatch.vkDestroyPipelineLayout(dev.handle, pipeLayout, nullptr);
}

// Renderer
Renderer::~Renderer() {
	if(!dev_) {
		return;
	}

	auto vkDev = dev_->handle;
	if(font_.uploadBuf) {
		dev_->dispatch.vkDestroyBuffer(vkDev, font_.uploadBuf, nullptr);
	}

	if(font_.uploadMem) {
		dev_->dispatch.vkFreeMemory(vkDev, font_.uploadMem, nullptr);
	}

	if(font_.view) {
		dev_->dispatch.vkDestroyImageView(vkDev, font_.view, nullptr);
	}

	if(font_.image) {
		dev_->dispatch.vkDestroyImage(vkDev, font_.image, nullptr);
	}

	if(font_.mem) {
		dev_->dispatch.vkFreeMemory(vkDev, font_.mem, nullptr);
	}
}

void Renderer::init(Device& dev, VkFormat format, bool clear) {
	this->dev_ = &dev;
	this->clear_ = clear;

	VkAttachmentDescription attachment = {};
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = clear ?
		VK_ATTACHMENT_LOAD_OP_CLEAR :
		VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = clear ?
		VK_IMAGE_LAYOUT_UNDEFINED :
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorAttachment = {};
	colorAttachment.attachment = 0;
	colorAttachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorAttachment;

	VkSubpassDependency dependencies[2] = {};

	// in-dependency
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	// out-dependency
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

	VkRenderPassCreateInfo rpi = vk::RenderPassCreateInfo();
	rpi.attachmentCount = 1;
	rpi.pAttachments = &attachment;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;
	rpi.dependencyCount = 2;
	rpi.pDependencies = dependencies;

	VK_CHECK(dev.dispatch.vkCreateRenderPass(dev.handle, &rpi, nullptr, &rp_));

	// pipeline
	VkShaderModule vertModule, fragModule;

	VkShaderModuleCreateInfo vertShaderInfo = vk::ShaderModuleCreateInfo();
	vertShaderInfo.codeSize = sizeof(overlay_vert_spv_data);
	vertShaderInfo.pCode = overlay_vert_spv_data;
	VK_CHECK(dev.dispatch.vkCreateShaderModule(dev.handle, &vertShaderInfo, NULL, &vertModule));

	VkShaderModuleCreateInfo fragShaderInfo = vk::ShaderModuleCreateInfo();
	fragShaderInfo.codeSize = sizeof(overlay_frag_spv_data);
	fragShaderInfo.pCode = overlay_frag_spv_data;
	VK_CHECK(dev.dispatch.vkCreateShaderModule(dev.handle, &fragShaderInfo, NULL, &fragModule));

	VkPipelineShaderStageCreateInfo stage[2] = {};
	stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage[0].module = vertModule;
	stage[0].pName = "main";

	stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stage[1].module = fragModule;
	stage[1].pName = "main";

	VkVertexInputBindingDescription bindingDesc[1] = {};
	bindingDesc[0].stride = sizeof(ImDrawVert);
	bindingDesc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attribDesc[3] = {};
	attribDesc[0].location = 0;
	attribDesc[0].binding = bindingDesc[0].binding;
	attribDesc[0].format = VK_FORMAT_R32G32_SFLOAT;
	attribDesc[0].offset = offsetof(ImDrawVert, pos);

	attribDesc[1].location = 1;
	attribDesc[1].binding = bindingDesc[0].binding;
	attribDesc[1].format = VK_FORMAT_R32G32_SFLOAT;
	attribDesc[1].offset = offsetof(ImDrawVert, uv);

	attribDesc[2].location = 2;
	attribDesc[2].binding = bindingDesc[0].binding;
	attribDesc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
	attribDesc[2].offset = offsetof(ImDrawVert, col);

	VkPipelineVertexInputStateCreateInfo vertexInfo = vk::PipelineVertexInputStateCreateInfo();
	vertexInfo.vertexBindingDescriptionCount = 1;
	vertexInfo.pVertexBindingDescriptions = bindingDesc;
	vertexInfo.vertexAttributeDescriptionCount = 3;
	vertexInfo.pVertexAttributeDescriptions = attribDesc;

	VkPipelineInputAssemblyStateCreateInfo iaInfo = vk::PipelineInputAssemblyStateCreateInfo();
	iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportInfo = vk::PipelineViewportStateCreateInfo();
	viewportInfo.viewportCount = 1;
	viewportInfo.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterInfo = vk::PipelineRasterizationStateCreateInfo();
	rasterInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterInfo.cullMode = VK_CULL_MODE_NONE;
	rasterInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterInfo.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo msInfo = vk::PipelineMultisampleStateCreateInfo();
	msInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorAttach[1] = {};
	colorAttach[0].blendEnable = VK_TRUE;
	colorAttach[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorAttach[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorAttach[0].colorBlendOp = VK_BLEND_OP_ADD;
	colorAttach[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorAttach[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorAttach[0].alphaBlendOp = VK_BLEND_OP_ADD;
	colorAttach[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineDepthStencilStateCreateInfo depthInfo = vk::PipelineDepthStencilStateCreateInfo();

	VkPipelineColorBlendStateCreateInfo blendInfo = vk::PipelineColorBlendStateCreateInfo();
	blendInfo.attachmentCount = 1;
	blendInfo.pAttachments = colorAttach;

	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state = vk::PipelineDynamicStateCreateInfo();
	dynamic_state.dynamicStateCount = 2;
	dynamic_state.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo gpi = {};
	gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi.flags = 0;
	gpi.stageCount = 2;
	gpi.pStages = stage;
	gpi.pVertexInputState = &vertexInfo;
	gpi.pInputAssemblyState = &iaInfo;
	gpi.pViewportState = &viewportInfo;
	gpi.pRasterizationState = &rasterInfo;
	gpi.pMultisampleState = &msInfo;
	gpi.pDepthStencilState = &depthInfo;
	gpi.pColorBlendState = &blendInfo;
	gpi.pDynamicState = &dynamic_state;
	gpi.layout = dev.renderData->pipeLayout;
	gpi.renderPass = rp_;
	VK_CHECK(dev.dispatch.vkCreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe_));

	dev.dispatch.vkDestroyShaderModule(dev.handle, vertModule, nullptr);
	dev.dispatch.vkDestroyShaderModule(dev.handle, fragModule, nullptr);
}

void Renderer::ensureFontAtlas(VkCommandBuffer cb) {
	if(font_.uploaded) {
		return;
	}

	auto& dev = *this->dev_;

	ImGuiIO& io = ImGui::GetIO();
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	size_t uploadSize = width * height * 4 * sizeof(char);

	// Create atlas image
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent.width = width;
	ici.extent.height = height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VK_CHECK(dev.dispatch.vkCreateImage(dev.handle, &ici, nullptr, &font_.image));

	VkMemoryRequirements fontImageReq;
	dev.dispatch.vkGetImageMemoryRequirements(dev.handle, font_.image, &fontImageReq);

	VkMemoryAllocateInfo iai = {};
	iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	iai.allocationSize = fontImageReq.size;
	iai.memoryTypeIndex = findLSB(fontImageReq.memoryTypeBits & dev.deviceLocalMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.handle, &iai, nullptr, &font_.mem));
	VK_CHECK(dev.dispatch.vkBindImageMemory(dev.handle, font_.image, font_.mem, 0));

	// font image view
	VkImageViewCreateInfo ivi = {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = font_.image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.levelCount = 1;
	ivi.subresourceRange.layerCount = 1;
	VK_CHECK(dev.dispatch.vkCreateImageView(dev.handle, &ivi, nullptr, &font_.view));

	// Create the upload buffer
	VkBufferCreateInfo bci = vk::BufferCreateInfo();
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = uploadSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VK_CHECK(dev.dispatch.vkCreateBuffer(dev.handle, &bci, nullptr, &font_.uploadBuf));

	VkMemoryRequirements uploadBufReq;
	dev.dispatch.vkGetBufferMemoryRequirements(dev.handle, font_.uploadBuf, &uploadBufReq);

	VkMemoryAllocateInfo uploadai = {};
	uploadai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	uploadai.allocationSize = uploadBufReq.size;
	uploadai.memoryTypeIndex = findLSB(uploadBufReq.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.handle, &uploadai, nullptr, &font_.uploadMem));
	VK_CHECK(dev.dispatch.vkBindBufferMemory(dev.handle, font_.uploadBuf, font_.uploadMem, 0));

	// Upload to Buffer
	char* map = NULL;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.handle, font_.uploadMem, 0, uploadSize, 0, (void**)(&map)));
	std::memcpy(map, pixels, uploadSize);

	VkMappedMemoryRange range[1] = {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = font_.uploadMem;
	range[0].size = uploadSize;
	VK_CHECK(dev.dispatch.vkFlushMappedMemoryRanges(dev.handle, 1, range));
	dev.dispatch.vkUnmapMemory(dev.handle, font_.uploadMem);

	// Copy buffer to image
	VkImageMemoryBarrier copyBarrier[1] = {};
	copyBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	copyBarrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	copyBarrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	copyBarrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	copyBarrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copyBarrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copyBarrier[0].image = font_.image;
	copyBarrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyBarrier[0].subresourceRange.levelCount = 1;
	copyBarrier[0].subresourceRange.layerCount = 1;
	dev.dispatch.vkCmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_HOST_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL,
		1, copyBarrier);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;
	dev.dispatch.vkCmdCopyBufferToImage(cb,
		font_.uploadBuf,
		font_.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region);

	VkImageMemoryBarrier useBarrier[1] = {};
	useBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	useBarrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	useBarrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	useBarrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	useBarrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	useBarrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	useBarrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	useBarrier[0].image = font_.image;
	useBarrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	useBarrier[0].subresourceRange.levelCount = 1;
	useBarrier[0].subresourceRange.layerCount = 1;
	dev.dispatch.vkCmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0,
		0, NULL,
		0, NULL,
		1, useBarrier);

	// create descriptor
	VkDescriptorSetAllocateInfo dsai = vk::DescriptorSetAllocateInfo();
	dsai.descriptorPool = dev.dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &dev_->renderData->dsLayout;
	VK_CHECK(dev.dispatch.vkAllocateDescriptorSets(dev.handle, &dsai, &dsFont_));

	// ...and update it
	VkDescriptorImageInfo dsii;
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = font_.view;
	dsii.sampler = dev_->renderData->linearSampler;

	VkWriteDescriptorSet write = vk::WriteDescriptorSet();
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = dsFont_;
	write.pImageInfo = &dsii;

	dev.dispatch.vkUpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);

	// Store our identifier
	io.Fonts->TexID = (ImTextureID) dsFont_;
	font_.uploaded = true;
}

void Renderer::uploadDraw(Draw& draw, const ImDrawData& drawData) {
	auto& dev = *this->dev_;
	if(drawData.TotalIdxCount == 0) {
		return;
	}

	// make sure buffers are large enough
	auto vertexSize = drawData.TotalVtxCount * sizeof(ImDrawVert);
	auto vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	draw.vertexBuffer.ensure(dev, vertexSize, vertexUsage);

	auto indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	auto indexSize = drawData.TotalIdxCount * sizeof(ImDrawIdx);
	draw.indexBuffer.ensure(dev, indexSize, indexUsage);

	// map
	ImDrawVert* verts;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.handle, draw.vertexBuffer.mem, 0, vertexSize, 0, (void**) &verts));

	ImDrawIdx* inds;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.handle, draw.indexBuffer.mem, 0, indexSize, 0, (void**) &inds));

	for(auto i = 0; i < drawData.CmdListsCount; ++i) {
		auto& cmds = *drawData.CmdLists[i];
		std::memcpy(verts, cmds.VtxBuffer.Data, cmds.VtxBuffer.size() * sizeof(ImDrawVert));
		std::memcpy(inds, cmds.IdxBuffer.Data, cmds.IdxBuffer.size() * sizeof(ImDrawIdx));
		verts += cmds.VtxBuffer.Size;
		inds += cmds.IdxBuffer.Size;
	}

	VkMappedMemoryRange range[2] = {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = draw.vertexBuffer.mem;
	range[0].size = VK_WHOLE_SIZE;
	range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[1].memory = draw.indexBuffer.mem;
	range[1].size = VK_WHOLE_SIZE;

	VK_CHECK(dev.dispatch.vkFlushMappedMemoryRanges(dev.handle, 2, range));
	dev.dispatch.vkUnmapMemory(dev.handle, draw.vertexBuffer.mem);
	dev.dispatch.vkUnmapMemory(dev.handle, draw.indexBuffer.mem);
}

void Renderer::recordDraw(Draw& draw, VkExtent2D extent, VkFramebuffer fb,
		bool force, const ImDrawData& drawData) {
	auto& dev = *dev_;
	ensureFontAtlas(draw.cb);
	if(drawData.TotalIdxCount == 0 && !force) {
		return;
	}

	VkRenderPassBeginInfo rpBegin = vk::RenderPassBeginInfo();
	rpBegin.renderArea.extent = extent;
	rpBegin.renderPass = rp_;
	rpBegin.framebuffer = fb;

	VkClearValue clearValue;
	if(clear_) {
		clearValue.color = {{0.f, 0.f, 0.f, 1.f}};
		rpBegin.clearValueCount = 1u;
		rpBegin.pClearValues = &clearValue;
	}

	dev.dispatch.vkCmdBeginRenderPass(draw.cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

	if(drawData.TotalIdxCount > 0) {
		VkViewport viewport {};
		viewport.width = extent.width;
		viewport.height = extent.height;
		viewport.maxDepth = 1.f;
		dev.dispatch.vkCmdSetViewport(draw.cb, 0, 1, &viewport);

		dev.dispatch.vkCmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);

		VkDeviceSize off0 = 0u;
		dev.dispatch.vkCmdBindVertexBuffers(draw.cb, 0, 1, &draw.vertexBuffer.buf, &off0);
		dev.dispatch.vkCmdBindIndexBuffer(draw.cb, draw.indexBuffer.buf, 0, VK_INDEX_TYPE_UINT16);

		float pcr[4];
		// scale
		pcr[0] = 2.0f / drawData.DisplaySize.x;
		pcr[1] = 2.0f / drawData.DisplaySize.y;
		// translate
		pcr[2] = -1.0f - drawData.DisplayPos.x * pcr[0];
		pcr[3] = -1.0f - drawData.DisplayPos.y * pcr[1];
		dev.dispatch.vkCmdPushConstants(draw.cb, dev.renderData->pipeLayout,
			VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pcr), pcr);

		auto idxOff = 0u;
		auto vtxOff = 0u;
		for(auto i = 0; i < drawData.CmdListsCount; ++i) {
			auto& cmds = *drawData.CmdLists[i];

			for(auto j = 0; j < cmds.CmdBuffer.Size; ++j) {
				auto& cmd = cmds.CmdBuffer[j];

				auto ds = (VkDescriptorSet) cmd.TextureId;
				if(!ds) {
					// we have to always bind a valid ds
					ds = dsFont_;
				}
				dev.dispatch.vkCmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					dev.renderData->pipeLayout, 0, 1, &ds, 0, nullptr);

				// TODO?
				VkRect2D scissor {};
				scissor.offset.x = std::max<int>(cmd.ClipRect.x - drawData.DisplayPos.x, 0);
				scissor.offset.y = std::max<int>(cmd.ClipRect.y - drawData.DisplayPos.y, 0);
				scissor.extent.width = cmd.ClipRect.z - cmd.ClipRect.x;
				scissor.extent.height = cmd.ClipRect.w - cmd.ClipRect.y;
				// scissor.extent.width = viewport.width;
				// scissor.extent.height = viewport.height;
				dev.dispatch.vkCmdSetScissor(draw.cb, 0, 1, &scissor);

				dev.dispatch.vkCmdDrawIndexed(draw.cb, cmd.ElemCount, 1, idxOff, vtxOff, 0);
				idxOff += cmd.ElemCount;
			}

			vtxOff += cmds.VtxBuffer.Size;
		}
	}

	dev.dispatch.vkCmdEndRenderPass(draw.cb);
}

} // namespace fuen
