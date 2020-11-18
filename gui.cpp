#include "gui.hpp"
#include "layer.hpp"
#include "data.hpp"
#include "swapchain.hpp"
#include "util.hpp"
#include "image.hpp"
#include "ds.hpp"
#include "sync.hpp"
#include "shader.hpp"
#include "pipe.hpp"
#include "buffer.hpp"
#include "cb.hpp"
#include "commands.hpp"
#include "rp.hpp"
#include "bytes.hpp"

#include "overlay.frag.spv.h"
#include "overlay.vert.spv.h"

#include <set>
#include <map>
#include <vkpp/names.hpp>
#include <vkpp/structs.hpp>
#include <imgui/imgui.h>

thread_local ImGuiContext* __LayerImGui;

namespace fuen {

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
	dsai.pSetLayouts = &dev.dsLayout;
	VK_CHECK(dev.dispatch.vkAllocateDescriptorSets(dev.handle, &dsai, &dsSelected));
}

// Overlay
void Overlay::init(Swapchain& swapchain) {
	this->swapchain = &swapchain;
	this->platform = findData<Platform>(swapchain.ci.surface);
	this->renderer.init(*swapchain.dev, swapchain.ci.imageFormat, false);

	auto& dev = *swapchain.dev;

	buffers.resize(swapchain.images.size());
	for(auto i = 0u; i < swapchain.images.size(); ++i) {
		buffers[i].init(dev, swapchain.images[i]->handle,
			swapchain.ci.imageFormat, swapchain.ci.imageExtent, renderer.rp);
	}
}

VkResult Overlay::drawPresent(Queue& queue, span<const VkSemaphore> semaphores,
		u32 imageIdx) {
	auto& dev = *queue.dev;

	// find free draw
	Draw* foundDraw = nullptr;
	for(auto& draw : draws) {
		if(dev.dispatch.vkGetFenceStatus(dev.handle, draw.fence) == VK_SUCCESS) {
			VK_CHECK(dev.dispatch.vkResetFences(dev.handle, 1, &draw.fence));

			foundDraw = &draw;
			break;
		}
	}

	if(!foundDraw) {
		foundDraw = &draws.emplace_back();
		*foundDraw = {};
		foundDraw->init(dev);
	}

	auto& draw = *foundDraw;

	VkCommandBufferBeginInfo cbBegin = vk::CommandBufferBeginInfo();
	VK_CHECK(dev.dispatch.vkBeginCommandBuffer(draw.cb, &cbBegin));

	ImGui::SetCurrentContext(renderer.imgui);
	ImGui::GetIO().DisplaySize.x = swapchain->ci.imageExtent.width;
	ImGui::GetIO().DisplaySize.y = swapchain->ci.imageExtent.height;
	renderer.ensureFontAtlas(draw.cb);

	// if there is a platform (for input stuff), update it
	if(platform) {
		platform->update();
	}

	// render gui
	renderer.drawGui(draw);
	renderer.uploadDraw(draw);
	renderer.recordDraw(draw, swapchain->ci.imageExtent, buffers[imageIdx].fb, false);

	dev.dispatch.vkEndCommandBuffer(draw.cb);

	// submit batch
	// TODO: handle case where application doesn't give us semaphore
	// (and different queues are used?)
	auto waitStages = std::make_unique<VkPipelineStageFlags[]>(semaphores.size());
	for(auto i = 0u; i < semaphores.size(); ++i) {
		waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	}

	VkSubmitInfo submitInfo = vk::SubmitInfo();
	submitInfo.commandBufferCount = 1u;
	submitInfo.pCommandBuffers = &draw.cb;
	submitInfo.signalSemaphoreCount = 1u;
	submitInfo.pSignalSemaphores = &draw.semaphore;
	submitInfo.pWaitDstStageMask = waitStages.get();
	submitInfo.pWaitSemaphores = semaphores.data();
	submitInfo.waitSemaphoreCount = semaphores.size();

	auto res = dev.dispatch.vkQueueSubmit(dev.gfxQueue->queue, 1u, &submitInfo, draw.fence);
	if(res != VK_SUCCESS) {
		dlg_error("vkSubmit error: {}", vk::name(vk::Result(res)));
		return res;
	}

	// call down
	VkPresentInfoKHR presentInfo = vk::PresentInfoKHR();
	presentInfo.pImageIndices = &imageIdx;
	presentInfo.pWaitSemaphores = &draw.semaphore;
	presentInfo.waitSemaphoreCount = 1u;
	presentInfo.pSwapchains = &swapchain->handle;
	presentInfo.swapchainCount = 1u;
	// TODO: might be bad to not forward this
	// pi.pNext

	return dev.dispatch.vkQueuePresentKHR(queue.queue, &presentInfo);
}

// Renderer
Renderer::~Renderer() {
	if(!dev) {
		return;
	}

	// no need to free descriptor sets i guess

	if(imgui) {
		ImGui::DestroyContext(imgui);
	}

	if(selected.image.view) {
		dev->dispatch.vkDestroyImageView(dev->handle,
			selected.image.view, nullptr);
	}

	if(font.uploadBuf) {
		dev->dispatch.vkDestroyBuffer(dev->handle, font.uploadBuf, nullptr);
	}

	if(font.uploadMem) {
		dev->dispatch.vkFreeMemory(dev->handle, font.uploadMem, nullptr);
	}

	if(font.view) {
		dev->dispatch.vkDestroyImageView(dev->handle, font.view, nullptr);
	}

	if(font.image) {
		dev->dispatch.vkDestroyImage(dev->handle, font.image, nullptr);
	}

	if(font.mem) {
		dev->dispatch.vkFreeMemory(dev->handle, font.mem, nullptr);
	}
}

void Renderer::init(Device& dev, VkFormat format, bool clear) {
	this->dev = &dev;
	this->clear = clear;

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

	VK_CHECK(dev.dispatch.vkCreateRenderPass(dev.handle, &rpi, nullptr, &rp));

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
	gpi.layout = dev.pipeLayout;
	gpi.renderPass = rp;
	VK_CHECK(dev.dispatch.vkCreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));

	dev.dispatch.vkDestroyShaderModule(dev.handle, vertModule, nullptr);
	dev.dispatch.vkDestroyShaderModule(dev.handle, fragModule, nullptr);

	// setup imgui
	this->imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(imgui);
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetStyle().WindowRounding = 0.f;
	ImGui::GetStyle().WindowBorderSize = 0.f;
}

void Renderer::ensureFontAtlas(VkCommandBuffer cb) {
	if(font.uploaded) {
		return;
	}

	auto& dev = *this->dev;

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

	VK_CHECK(dev.dispatch.vkCreateImage(dev.handle, &ici, nullptr, &font.image));

	VkMemoryRequirements fontImageReq;
	dev.dispatch.vkGetImageMemoryRequirements(dev.handle, font.image, &fontImageReq);

	VkMemoryAllocateInfo iai = {};
	iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	iai.allocationSize = fontImageReq.size;
	iai.memoryTypeIndex = findLSB(fontImageReq.memoryTypeBits & dev.deviceLocalMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.handle, &iai, nullptr, &font.mem));
	VK_CHECK(dev.dispatch.vkBindImageMemory(dev.handle, font.image, font.mem, 0));

	// font image view
	VkImageViewCreateInfo ivi = {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = font.image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.levelCount = 1;
	ivi.subresourceRange.layerCount = 1;
	VK_CHECK(dev.dispatch.vkCreateImageView(dev.handle, &ivi, nullptr, &font.view));

	// Create the upload buffer
	VkBufferCreateInfo bci = vk::BufferCreateInfo();
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = uploadSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VK_CHECK(dev.dispatch.vkCreateBuffer(dev.handle, &bci, nullptr, &font.uploadBuf));

	VkMemoryRequirements uploadBufReq;
	dev.dispatch.vkGetBufferMemoryRequirements(dev.handle, font.uploadBuf, &uploadBufReq);

	VkMemoryAllocateInfo uploadai = {};
	uploadai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	uploadai.allocationSize = uploadBufReq.size;
	uploadai.memoryTypeIndex = findLSB(uploadBufReq.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.vkAllocateMemory(dev.handle, &uploadai, nullptr, &font.uploadMem));
	VK_CHECK(dev.dispatch.vkBindBufferMemory(dev.handle, font.uploadBuf, font.uploadMem, 0));

	// Upload to Buffer
	char* map = NULL;
	VK_CHECK(dev.dispatch.vkMapMemory(dev.handle, font.uploadMem, 0, uploadSize, 0, (void**)(&map)));
	std::memcpy(map, pixels, uploadSize);

	VkMappedMemoryRange range[1] = {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = font.uploadMem;
	range[0].size = uploadSize;
	VK_CHECK(dev.dispatch.vkFlushMappedMemoryRanges(dev.handle, 1, range));
	dev.dispatch.vkUnmapMemory(dev.handle, font.uploadMem);

	// Copy buffer to image
	VkImageMemoryBarrier copyBarrier[1] = {};
	copyBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	copyBarrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	copyBarrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	copyBarrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	copyBarrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copyBarrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	copyBarrier[0].image = font.image;
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
		font.uploadBuf,
		font.image,
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
	useBarrier[0].image = font.image;
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
	dsai.pSetLayouts = &dev.dsLayout;
	VK_CHECK(dev.dispatch.vkAllocateDescriptorSets(dev.handle, &dsai, &dsFont));

	// ...and update it
	VkDescriptorImageInfo dsii;
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = font.view;

	VkWriteDescriptorSet write = vk::WriteDescriptorSet();
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = dsFont;
	write.pImageInfo = &dsii;

	dev.dispatch.vkUpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);

	// Store our identifier
	io.Fonts->TexID = (ImTextureID) dsFont;
	font.uploaded = true;
}

void Renderer::drawOverviewUI(Draw& draw) {
	(void) draw;

	auto& dev = *this->dev;

	// instance info
	ImGui::Columns(2);

	ImGui::Text("API Version");
	ImGui::Text("Application");
	ImGui::Text("Engine");

	ImGui::NextColumn();

	auto& ini = *dev.ini;
	ImGui::Text("%d.%d.%d",
		VK_VERSION_MAJOR(ini.app.apiVersion),
		VK_VERSION_MINOR(ini.app.apiVersion),
		VK_VERSION_PATCH(ini.app.apiVersion));
	ImGui::Text("%s %d.%d.%d", ini.app.name.c_str(),
		VK_VERSION_MAJOR(ini.app.version),
		VK_VERSION_MINOR(ini.app.version),
		VK_VERSION_PATCH(ini.app.version));
	ImGui::Text("%s %d.%d.%d", ini.app.engineName.c_str(),
		VK_VERSION_MAJOR(ini.app.engineVersion),
		VK_VERSION_MINOR(ini.app.engineVersion),
		VK_VERSION_PATCH(ini.app.engineVersion));

	ImGui::Columns();

	ImGui::Separator();

	// phdev info
	ImGui::Columns(2);

	// physical device info
	VkPhysicalDeviceProperties phProps;
	dev.dispatch.vkGetPhysicalDeviceProperties(dev.phdev, &phProps);

	ImGui::Text("Physical device, API version");
	ImGui::Text("Driver version");

	ImGui::NextColumn();

	ImGui::Text("%s %d.%d.%d", phProps.deviceName,
		VK_VERSION_MAJOR(phProps.apiVersion),
		VK_VERSION_MINOR(phProps.apiVersion),
		VK_VERSION_PATCH(phProps.apiVersion));
	ImGui::Text("%d.%d.%d",
		VK_VERSION_MAJOR(phProps.driverVersion),
		VK_VERSION_MINOR(phProps.driverVersion),
		VK_VERSION_PATCH(phProps.driverVersion));

	ImGui::Columns();

	// pretty much just own debug stuff
	ImGui::Separator();

	ImGui::Columns(2);

	ImGui::Text("num submissions");

	ImGui::NextColumn();

	ImGui::Text("%u", u32(dev.pending.size()));

	ImGui::Columns();
}

template<typename T>
std::string name(const T& handle) {
	std::string name = handle.name;
	if(name.empty()) {
		name = dlg::format("{} {}{}", typeid(handle).name(),
			std::hex, handleToU64(handle.handle));
	}

	return name;
}

void Renderer::drawMemoryResourceUI(Draw&, MemoryResource& res) {
	if(res.memory) {
		ImGui::Text("Bound to memory ");
		ImGui::SameLine();
		auto label = name(*res.memory);
		if(ImGui::SmallButton(label.c_str())) {
			selected.handle = res.memory;
		}

		ImGui::SameLine();
		ImGui::Text(" (offset %lu, size %lu)",
			(unsigned long) res.allocationOffset,
			(unsigned long) res.allocationSize);
	}
}

void Renderer::drawResourceUI(Draw& draw, Image& image) {
	if(selected.image.handle != image.handle) {
		selected.image.handle = image.handle;

		if(selected.image.view) {
			dev->dispatch.vkDestroyImageView(dev->handle, selected.image.view, nullptr);
			selected.image.view = {};
		}

		if(!image.swapchain) {
			selected.image.aspectMask = isDepthFormat(vk::Format(image.ci.format)) ?
				VK_IMAGE_ASPECT_DEPTH_BIT :
				VK_IMAGE_ASPECT_COLOR_BIT;

			VkImageViewCreateInfo ivi = vk::ImageViewCreateInfo();
			ivi.image = image.handle;
			ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			ivi.format = image.ci.format;
			ivi.subresourceRange.aspectMask = selected.image.aspectMask;
			ivi.subresourceRange.layerCount = 1u;
			ivi.subresourceRange.levelCount = 1u;

			auto res = dev->dispatch.vkCreateImageView(dev->handle, &ivi, nullptr, &selected.image.view);
			dlg_assert(res == VK_SUCCESS);

			VkDescriptorImageInfo dsii;
			dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			dsii.imageView = selected.image.view;

			VkWriteDescriptorSet write = vk::WriteDescriptorSet();
			write.descriptorCount = 1u;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.dstSet = draw.dsSelected;
			write.pImageInfo = &dsii;

			dev->dispatch.vkUpdateDescriptorSets(dev->handle, 1, &write, 0, nullptr);
		} else {
			dlg_debug("not creating view due to swapchain image");
		}
	}

	ImGui::Text("%s", image.name.c_str());
	ImGui::Spacing();

	// info
	auto ci = bit_cast<vk::ImageCreateInfo>(image.ci);
	ImGui::Columns(2);

	ImGui::Text("Extent");
	ImGui::Text("Layers");
	ImGui::Text("Levels");
	ImGui::Text("Format");
	ImGui::Text("Usage");
	ImGui::Text("Tiling");
	ImGui::Text("Samples");
	ImGui::Text("Type");
	ImGui::Text("Flags");

	ImGui::NextColumn();

	ImGui::Text("%dx%dx%d", ci.extent.width, ci.extent.height, ci.extent.depth);
	ImGui::Text("%d", ci.arrayLayers);
	ImGui::Text("%d", ci.mipLevels);
	ImGui::Text("%s", vk::name(ci.format));
	ImGui::Text("%s", vk::name(ci.usage).c_str());
	ImGui::Text("%s", vk::name(ci.tiling));
	ImGui::Text("%s", vk::name(ci.samples));
	ImGui::Text("%s", vk::name(ci.imageType));
	ImGui::Text("%s", vk::name(ci.flags).c_str());

	ImGui::Columns();

	// content
	if(selected.image.view) {
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::Image((void*) draw.dsSelected, {400, 400});
	}

	// resource references
	ImGui::Spacing();
	drawMemoryResourceUI(draw, image);

	ImGui::Spacing();
	ImGui::Text("Image Views:");

	for(auto* view : image.views) {
		ImGui::Bullet();
		if(ImGui::SmallButton(name(*view).c_str())) {
			selected.handle = view;
		}
	}

	// TODO: pending layout?
}

void Renderer::drawResourceUI(Draw& draw, Buffer& buffer) {
	ImGui::Text("%s", name(buffer).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);

	ImGui::SetColumnWidth(0, 100);

	ImGui::Text("Size");
	ImGui::Text("Usage");

	ImGui::NextColumn();

	auto& ci = buffer.ci;
	ImGui::Text("%lu", ci.size);
	ImGui::Text("%s", vk::name(vk::BufferUsageFlags(vk::BufferUsageBits(ci.usage))).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResourceUI(draw, buffer);
}

void Renderer::drawResourceUI(Draw&, Sampler& sampler) {
	ImGui::Text("%s", name(sampler).c_str());
	ImGui::Spacing();
	auto ci = bit_cast<vk::SamplerCreateInfo>(sampler.ci);

	// names
	ImGui::Columns(2);

	ImGui::Text("Min Filter");
	ImGui::Text("Mag Filter");
	ImGui::Text("Mipmap Mode");
	ImGui::Text("Addressing U");
	ImGui::Text("Addressing V");
	ImGui::Text("Addressing W");
	ImGui::Text("Border Color");
	ImGui::Text("Unnormalized");
	ImGui::Text("min LOD");
	ImGui::Text("max LOD");

	if(ci.anisotropyEnable) {
		ImGui::Text("Max Anisotropy");
	}

	if(ci.compareEnable) {
		ImGui::Text("Compare Op");
	}

	// data
	ImGui::NextColumn();

	ImGui::Text("%s", vk::name(ci.minFilter));
	ImGui::Text("%s", vk::name(ci.magFilter));
	ImGui::Text("%s", vk::name(ci.mipmapMode));
	ImGui::Text("%s", vk::name(ci.addressModeU));
	ImGui::Text("%s", vk::name(ci.addressModeV));
	ImGui::Text("%s", vk::name(ci.addressModeW));
	ImGui::Text("%s", vk::name(ci.borderColor));
	ImGui::Text("%d", ci.unnormalizedCoordinates);
	ImGui::Text("%f", ci.minLod);
	ImGui::Text("%f", ci.maxLod);

	if(ci.anisotropyEnable) {
		ImGui::Text("%f", ci.maxAnisotropy);
	}

	if(ci.compareEnable) {
		ImGui::Text("%s", vk::name(ci.compareOp));
	}

	ImGui::Columns();
}

void Renderer::drawResourceUI(Draw&, DescriptorSet&) {
	ImGui::Text("TODO");
}

void Renderer::drawResourceUI(Draw&, DescriptorPool&) {
	ImGui::Text("TODO");
}

void Renderer::drawResourceUI(Draw&, DescriptorSetLayout& dsl) {
	ImGui::Text("%s", name(dsl).c_str());
	ImGui::Spacing();

	ImGui::Text("Bindings");

	for(auto& binding : dsl.bindings) {
		// TODO: immutable samplers
		if(binding.descriptorCount > 1) {
			ImGui::BulletText("%s[%d] in (%s)",
				vk::name(vk::DescriptorType(binding.descriptorType)),
				binding.descriptorCount,
				vk::name(vk::ShaderStageFlags(vk::ShaderStageBits(binding.stageFlags))).c_str());
		} else {
			ImGui::BulletText("%s in (%s)",
				vk::name(vk::DescriptorType(binding.descriptorType)),
				vk::name(vk::ShaderStageFlags(vk::ShaderStageBits(binding.stageFlags))).c_str());
		}
	}
}

void Renderer::drawResourceUI(Draw&, GraphicsPipeline& pipe) {
	ImGui::Text("%s", name(pipe).c_str());
	ImGui::Spacing();

	// general info
	// text
	ImGui::Columns(2);

	ImGui::Text("Layout");
	ImGui::Text("Render Pass");
	ImGui::Text("Subpass");

	// data
	ImGui::NextColumn();

	if(ImGui::SmallButton(name(*pipe.layout).c_str())) {
		selected.handle = pipe.layout;
	}
	if(ImGui::SmallButton(name(*pipe.renderPass).c_str())) {
		selected.handle = pipe.renderPass;
	}
	ImGui::Text("%d", pipe.subpass);

	ImGui::Columns();
	ImGui::Separator();

	// rasterization
	auto rastInfo = bit_cast<vk::PipelineRasterizationStateCreateInfo>(pipe.rasterizationState);

	ImGui::Text("Rasterization");
	ImGui::Columns(2);

	ImGui::Text("Discard");
	ImGui::Text("Depth Clamp");
	ImGui::Text("Cull Mode");
	ImGui::Text("Polygon Mode");
	ImGui::Text("Front Face");

	if(rastInfo.depthBiasEnable) {
		ImGui::Text("Depth Bias Constant");
		ImGui::Text("Depth Bias Slope");
		ImGui::Text("Depth Bias Clamp");
	}

	ImGui::NextColumn();

	ImGui::Text("%d", rastInfo.rasterizerDiscardEnable);
	ImGui::Text("%d", rastInfo.depthClampEnable);

	ImGui::Text("%s", vk::name(rastInfo.cullMode).c_str());
	ImGui::Text("%s", vk::name(rastInfo.polygonMode));
	ImGui::Text("%s", vk::name(rastInfo.frontFace));

	if(rastInfo.depthBiasEnable) {
		ImGui::Text("%f", rastInfo.depthBiasSlopeFactor);
		ImGui::Text("%f", rastInfo.depthBiasConstantFactor);
		ImGui::Text("%f", rastInfo.depthBiasClamp);
	}

	ImGui::Columns();
	ImGui::Separator();

	if(!pipe.hasMeshShader) {
		// input assembly
		ImGui::Text("Input Assembly");

		ImGui::Columns(2);
		ImGui::Separator();

		ImGui::Text("Primitive restart");
		ImGui::Text("Topology");

		ImGui::NextColumn();

		ImGui::Text("%d", pipe.inputAssemblyState.primitiveRestartEnable);
		ImGui::Text("%s", vk::name(vk::PrimitiveTopology(pipe.inputAssemblyState.topology)));

		ImGui::Columns();
		ImGui::Separator();

		// vertex input
		if(pipe.vertexInputState.vertexAttributeDescriptionCount > 0) {
			ImGui::Text("Vertex input");

			std::map<u32, u32> bindings;
			for(auto i = 0u; i < pipe.vertexInputState.vertexBindingDescriptionCount; ++i) {
				auto& binding = pipe.vertexInputState.pVertexBindingDescriptions[i];
				bindings[binding.binding] = i;
			}

			std::map<u32, u32> attribs;
			for(auto bid : bindings) {
				auto& binding = pipe.vertexInputState.pVertexBindingDescriptions[bid.second];

				ImGui::BulletText("Binding %d, %s, stride %d", binding.binding,
					vk::name(vk::VertexInputRate(binding.inputRate)), binding.stride);

				attribs.clear();
				for(auto i = 0u; i < pipe.vertexInputState.vertexAttributeDescriptionCount; ++i) {
					auto& attrib = pipe.vertexInputState.pVertexAttributeDescriptions[i];
					if(attrib.binding != binding.binding) {
						continue;
					}

					attribs[attrib.location] = i;
				}

				ImGui::Indent();

				for(auto& aid : attribs) {
					auto& attrib = pipe.vertexInputState.pVertexAttributeDescriptions[aid.second];
					ImGui::BulletText("location %d at offset %d, %s",
						attrib.location, attrib.offset, vk::name(vk::Format(attrib.format)));
				}

				ImGui::Unindent();
			}

			ImGui::Separator();
		}
	}

	if(!pipe.dynamicState.empty()) {
		ImGui::Text("Dynamic states");

		for(auto& dynState : pipe.dynamicState) {
			ImGui::BulletText("%s", vk::name(vk::DynamicState(dynState)));
		}

		ImGui::Separator();
	}

	if(!pipe.rasterizationState.rasterizerDiscardEnable) {
		if(pipe.multisampleState.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
			ImGui::Text("Multisample state");

			ImGui::Columns(2);

			ImGui::Text("Samples");
			ImGui::Text("Sample Shading");
			ImGui::Text("Min Sample Shading");
			ImGui::Text("Alpha To One");
			ImGui::Text("Alpha To Coverage");

			ImGui::NextColumn();

			ImGui::Text("%s", vk::name(vk::SampleCountBits(pipe.multisampleState.rasterizationSamples)));
			ImGui::Text("%d", pipe.multisampleState.sampleShadingEnable);
			ImGui::Text("%f", pipe.multisampleState.minSampleShading);
			ImGui::Text("%d", pipe.multisampleState.alphaToOneEnable);
			ImGui::Text("%d", pipe.multisampleState.alphaToCoverageEnable);

			// TODO: sample mask

			ImGui::Columns();
			ImGui::Separator();
		}

		// TODO: viewport & scissors

		if(pipe.hasDepthStencil) {
			ImGui::Text("Depth stencil");

			ImGui::Columns(2);

			ImGui::Text("Depth Test Enable");
			ImGui::Text("Depth Write Enable");

			if(pipe.depthStencilState.depthTestEnable) {
				ImGui::Text("Depth Compare Op");
				if(pipe.depthStencilState.depthBoundsTestEnable) {
					ImGui::Text("Min Depth Bounds");
					ImGui::Text("Max Depth Bounds");
				}
			}

			ImGui::Text("Stencil Test Enable");
			if(pipe.depthStencilState.stencilTestEnable) {
			}

			// Data
			ImGui::NextColumn();

			ImGui::Text("%d", pipe.depthStencilState.depthTestEnable);
			ImGui::Text("%d", pipe.depthStencilState.depthWriteEnable);
			ImGui::Text("%d", pipe.depthStencilState.stencilTestEnable);

			if(pipe.depthStencilState.depthTestEnable) {
				ImGui::Text("%s", vk::name(vk::CompareOp(pipe.depthStencilState.depthCompareOp)));

				if(pipe.depthStencilState.depthBoundsTestEnable) {
					ImGui::Text("%f", pipe.depthStencilState.minDepthBounds);
					ImGui::Text("%f", pipe.depthStencilState.maxDepthBounds);
				}
			}

			/*
			// TODO: stencil info
			if(pipe.depthStencilState.stencilTestEnable) {
				auto printStencilValues = [&](const VkStencilOpState& stencil) {
				};

				if(ImGui::TreeNode("Stencil Front")) {
					printStencilState(pipe.depthStencilState.front);
				}

				if(ImGui::TreeNode("Stencil Back")) {
					printStencilState(pipe.depthStencilState.back);
				}
			}
			*/

			ImGui::Columns();
			ImGui::Separator();
		}
	}

	// TODO: shader data
	// TODO: color blend state
	// TODO: tesselation
}

void Renderer::drawResourceUI(Draw&, ComputePipeline&) {
	ImGui::Text("TODO");
}

void Renderer::drawResourceUI(Draw&, PipelineLayout& pipeLayout) {
	ImGui::Text("%s", name(pipeLayout).c_str());
	ImGui::Spacing();

	if(!pipeLayout.pushConstants.empty()) {
		ImGui::Text("Push Constants");
		for(auto& pcr : pipeLayout.pushConstants) {
			ImGui::Bullet();
			ImGui::Text("Offset %d, Size %d, in %s", pcr.offset, pcr.size,
				vk::name(vk::ShaderStageFlags(vk::ShaderStageBits(pcr.stageFlags))).c_str());
		}
	}

	ImGui::Text("Descriptor Set Layouts");
	for(auto* ds : pipeLayout.descriptors) {
		ImGui::Bullet();
		if(ImGui::SmallButton(name(*ds).c_str())) {
			selected.handle = ds;
		}
	}
}
void Renderer::drawResourceUI(Draw&, CommandPool&) {
	ImGui::Text("TODO");
}

void Renderer::drawResourceUI(Draw&, DeviceMemory& mem) {
	ImGui::Text("%s", name(mem).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);

	ImGui::Text("Size");
	ImGui::Text("Type Index");

	// data
	ImGui::NextColumn();

	ImGui::Text("%lu", mem.size);
	ImGui::Text("%u", mem.typeIndex);

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	ImGui::Text("Bound Resources:");

	ImGui::Columns(3);
	ImGui::SetColumnWidth(0, 100);
	ImGui::SetColumnWidth(1, 300);

	for(auto& resource : mem.allocations) {
		ImGui::Text("%lu: ", resource.offset);

		ImGui::NextColumn();

		if(resource.resource->memoryResourceType == MemoryResource::Type::buffer) {
			Buffer& buffer = static_cast<Buffer&>(*resource.resource);
			auto label = name(buffer);
			ImGui::SmallButton(label.c_str());
		} else if(resource.resource->memoryResourceType == MemoryResource::Type::image) {
			Image& img = static_cast<Image&>(*resource.resource);
			auto label = name(img);
			ImGui::SmallButton(label.c_str());
		}

		ImGui::NextColumn();
		ImGui::Text("size %lu", resource.size);

		ImGui::NextColumn();
	}

	ImGui::Columns();
}

void Renderer::drawResourceUI(Draw&, CommandBuffer& cb) {
	// make sure command buffer isn't changed in the meantime
	std::lock_guard lock(cb.mutex);

	ImGui::Text("%s", name(cb).c_str());
	ImGui::Spacing();

	// TODO: more info about cb

	ImGui::Text("Pool: ");
	ImGui::SameLine();
	if(ImGui::SmallButton(name(*cb.pool).c_str())) {
		selected.handle = cb.pool;
	}

	// maybe show commands inline (in tree node)
	// and allow via button to switch to cb viewer?
	if(ImGui::Button("View Content")) {
		selected.cb.selectTabCounter = 0u;
		selected.cb.cb = &cb;
	}
}

void imguiPrintRange(u32 base, u32 count) {
	if(count > 1) {
		ImGui::Text("[%d, %d]", base, base + count - 1);
	} else {
		ImGui::Text("%d", base);
	}
}

void Renderer::drawResourceUI(Draw&, ImageView& view) {
	ImGui::Text("%s", name(view).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);
	auto ci = bit_cast<vk::ImageViewCreateInfo>(view.ci);

	ImGui::Text("Image");
	ImGui::Text("Type");
	ImGui::Text("Layers");
	ImGui::Text("Levels");
	ImGui::Text("Aspect");
	ImGui::Text("Format");
	ImGui::Text("Flags");

	// data
	ImGui::NextColumn();

	if(ImGui::SmallButton(name(*view.img).c_str())) {
		selected.handle = view.img;
	}

	ImGui::Text("%s", vk::name(ci.viewType));
	imguiPrintRange(ci.subresourceRange.baseArrayLayer, ci.subresourceRange.layerCount);
	imguiPrintRange(ci.subresourceRange.baseMipLevel, ci.subresourceRange.levelCount);
	ImGui::Text("%s", vk::name(ci.subresourceRange.aspectMask).c_str());
	ImGui::Text("%s", vk::name(ci.format));
	ImGui::Text("%s", vk::name(ci.flags).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	if(!view.fbs.empty()) {
		ImGui::Text("Framebuffers:");

		for(auto* fb : view.fbs) {
			ImGui::Bullet();
			if(ImGui::SmallButton(name(*fb).c_str())) {
				selected.handle = fb;
			}
		}
	}
}

void Renderer::drawResourceUI(Draw&, ShaderModule&) {
	ImGui::Text("TODO");
}

void Renderer::drawResourceUI(Draw&, Framebuffer& fb) {
	ImGui::Text("%s", name(fb).c_str());
	ImGui::Spacing();

	// TODO: info

	// Resource references
	ImGui::Spacing();
	ImGui::Text("Attachments:");

	for(auto* view : fb.attachments) {
		ImGui::Bullet();
		if(ImGui::SmallButton(name(*view).c_str())) {
			selected.handle = view;
		}
	}
}

void Renderer::drawResourceUI(Draw&, RenderPass&) {
	ImGui::Text("TODO");
}

void Renderer::drawCommandBufferInspector(Draw&, CommandBuffer& cb) {
	// make sure command buffer isn't changed in the meantime
	std::lock_guard lock(cb.mutex);

	// Command list
	ImGui::BeginChild("Command list", {400, 0});
	ImGui::PushID(dlg::format("{}:{}", &cb, cb.resetCount).c_str());
	displayCommands(cb.commands);
	ImGui::PopID();
	ImGui::EndChild();
	ImGui::SameLine();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	ImGui::Text("TODO: information about selected command");
	ImGui::EndChild();
}

void Renderer::drawResourceSelectorUI(Draw& draw) {
	// resource list
	ImGui::BeginChild("Resource List", {300, 0.f});

	auto displayResources = [&](auto& resMap) {
		for(auto& entry : resMap.map) {
			ImGui::PushID(entry.second.get());

			auto label = name(*entry.second);
			if(ImGui::Button(label.c_str())) {
				selected.handle = entry.second.get();
			}

			ImGui::PopID();
		}
	};

	displayResources(dev->images);
	displayResources(dev->imageViews);
	displayResources(dev->samplers);
	displayResources(dev->framebuffers);
	displayResources(dev->renderPasses);
	displayResources(dev->buffers);
	displayResources(dev->deviceMemories);
	displayResources(dev->commandBuffers);
	displayResources(dev->commandPools);
	displayResources(dev->dsPools);
	displayResources(dev->descriptorSets);
	displayResources(dev->dsLayouts);
	displayResources(dev->graphicsPipes);
	displayResources(dev->computePipes);
	displayResources(dev->pipeLayouts);
	displayResources(dev->shaderModules);

	ImGui::EndChild();

	// resource view
	ImGui::SameLine();
	ImGui::BeginChild("Resource View", {0.f, 0.f});

	std::visit(Visitor{
		[&](std::monostate) {},
		[&](auto* selected) {
			ImGui::PushID(selected);
			drawResourceUI(draw, *selected);
			ImGui::PopID();
		}
	}, selected.handle);

	ImGui::EndChild();
}

void Renderer::drawGui(Draw& draw) {
	ImGui::NewFrame();

	ImGui::ShowDemoWindow();
	ImGui::ShowAboutWindow();
	ImGui::ShowMetricsWindow();

	// ImGui::SetNextWindowPos({0, 0});
	// ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	// auto flags = ImGuiWindowFlags_NoDecoration;
	auto flags = 0;

	std::shared_lock lock(dev->mutex);
	if(ImGui::Begin("Fuencaliente", nullptr, flags)) {
		if(ImGui::BeginTabBar("MainTabBar")) {
			if(ImGui::BeginTabItem("Overview")) {
				drawOverviewUI(draw);
				ImGui::EndTabItem();
			}

			if(ImGui::BeginTabItem("Resources")) {
				drawResourceSelectorUI(draw);
				ImGui::EndTabItem();
			}

			if(selected.cb.cb) {
				auto flags = (selected.cb.selectTabCounter < 2) ? ImGuiTabItemFlags_SetSelected : 0;
				++selected.cb.selectTabCounter;
				if(ImGui::BeginTabItem("Command Buffer", nullptr, flags)) {
					drawCommandBufferInspector(draw, *selected.cb.cb);
					ImGui::EndTabItem();
				}
			}

			ImGui::EndTabBar();
		}
	}

	ImGui::End();
	ImGui::EndFrame();
	ImGui::Render();
}

void Renderer::uploadDraw(Draw& draw) {
	auto& dev = *this->dev;
	auto& drawData = *ImGui::GetDrawData();
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

void Renderer::recordDraw(Draw& draw, VkExtent2D extent, VkFramebuffer fb, bool force) {
	auto& dev = *this->dev;
	auto& drawData = *ImGui::GetDrawData();
	if(drawData.TotalIdxCount == 0 && !force) {
		return;
	}

	VkRenderPassBeginInfo rpBegin = vk::RenderPassBeginInfo();
	rpBegin.renderArea.extent = extent;
	rpBegin.renderPass = rp;
	rpBegin.framebuffer = fb;

	VkClearValue clearValue;
	if(this->clear) {
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

		dev.dispatch.vkCmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

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
		dev.dispatch.vkCmdPushConstants(draw.cb, dev.pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pcr), pcr);

		auto idxOff = 0u;
		auto vtxOff = 0u;
		for(auto i = 0; i < drawData.CmdListsCount; ++i) {
			auto& cmds = *drawData.CmdLists[i];

			for(auto j = 0; j < cmds.CmdBuffer.Size; ++j) {
				auto& cmd = cmds.CmdBuffer[j];

				auto ds = (VkDescriptorSet) cmd.TextureId;
				if(!ds) {
					// we have to always bind a valid ds
					ds = dsFont;
				}
				dev.dispatch.vkCmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					dev.pipeLayout, 0, 1, &ds, 0, nullptr);

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

void Renderer::unselect(const Handle& handle) {
	// unselect handle
	auto same = std::visit(Visitor{
		[&](std::monostate) {
			return false;
		}, [&](auto& selected) {
			return static_cast<const Handle*>(selected) == &handle;
		}
	}, selected.handle);

	if(same) {
		selected.handle = {};
	}

	// special cases
	if(selected.cb.cb == &handle) {
		selected.cb.cb = {};
	}
}

} // namespace fuen
