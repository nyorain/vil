#include <gui/gui.hpp>
#include <layer.hpp>
#include <queue.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <util.hpp>
#include <handles.hpp>
#include <commands.hpp>
#include <bytes.hpp>
#include <imguiutil.hpp>
#include <guidraw.hpp>
#include <spirv_reflect.h>
#include <imgui/imgui.h>

#include <set>
#include <map>
#include <fstream>
#include <filesystem>

// kinda hacky, done for shaders
#include <gui.frag.spv.h>
#include <gui.vert.spv.h>

#include <image.frag.1DArray.spv.h>
#include <image.frag.2DArray.spv.h>
#include <image.frag.3D.spv.h>

thread_local ImGuiContext* __LayerImGui;

namespace fuen {

// Gui
void Gui::init(Device& dev, VkFormat format, bool clear) {
	dev_ = &dev;
	clear_ = clear;

	tabs_.cb.gui_ = this;
	tabs_.resources.gui_ = this;

	lastFrame_ = Clock::now();

	// init render stuff
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

	VkRenderPassCreateInfo rpi {};
	rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpi.attachmentCount = 1;
	rpi.pAttachments = &attachment;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;
	rpi.dependencyCount = 2;
	rpi.pDependencies = dependencies;

	VK_CHECK(dev.dispatch.CreateRenderPass(dev.handle, &rpi, nullptr, &rp_));

	// pipeline
	std::vector<VkShaderModule> modules;
	auto initStages = [&](span<const u32> vertSpv, span<const u32> fragSpv) {
		VkShaderModule vertModule, fragModule;

		VkShaderModuleCreateInfo vertShaderInfo {};
		vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		vertShaderInfo.codeSize = vertSpv.size() * 4;
		vertShaderInfo.pCode = vertSpv.data();
		VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &vertShaderInfo, NULL, &vertModule));

		VkShaderModuleCreateInfo fragShaderInfo {};
		fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		fragShaderInfo.codeSize = fragSpv.size() * 4;
		fragShaderInfo.pCode = fragSpv.data();
		VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &fragShaderInfo, NULL, &fragModule));

		// store them for destruction later on
		modules.push_back(vertModule);
		modules.push_back(fragModule);

		std::array<VkPipelineShaderStageCreateInfo, 2> ret {};
		ret[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ret[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		ret[0].module = vertModule;
		ret[0].pName = "main";

		ret[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ret[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		ret[1].module = fragModule;
		ret[1].pName = "main";

		return ret;
	};

	auto guiStages = initStages(gui_vert_spv_data, gui_frag_spv_data);
	auto image1DStages = initStages(gui_vert_spv_data, image_frag_1DArray_spv_data);
	auto image2DStages = initStages(gui_vert_spv_data, image_frag_2DArray_spv_data);
	auto image3DStages = initStages(gui_vert_spv_data, image_frag_3D_spv_data);

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

	VkPipelineVertexInputStateCreateInfo vertexInfo {};
	vertexInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInfo.vertexBindingDescriptionCount = 1;
	vertexInfo.pVertexBindingDescriptions = bindingDesc;
	vertexInfo.vertexAttributeDescriptionCount = 3;
	vertexInfo.pVertexAttributeDescriptions = attribDesc;

	VkPipelineInputAssemblyStateCreateInfo iaInfo {};
	iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportInfo {};
	viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportInfo.viewportCount = 1;
	viewportInfo.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterInfo {};
	rasterInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterInfo.cullMode = VK_CULL_MODE_NONE;
	rasterInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterInfo.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo msInfo {};
	msInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorAttach[1] {};
	colorAttach[0].blendEnable = VK_TRUE;
	colorAttach[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorAttach[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorAttach[0].colorBlendOp = VK_BLEND_OP_ADD;
	colorAttach[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorAttach[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorAttach[0].alphaBlendOp = VK_BLEND_OP_ADD;
	colorAttach[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineDepthStencilStateCreateInfo depthInfo {};
	depthInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

	VkPipelineColorBlendStateCreateInfo blendInfo {};
	blendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blendInfo.attachmentCount = 1;
	blendInfo.pAttachments = colorAttach;

	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynState {};
	dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo gpi[4] {};

	gpi[0].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi[0].flags = 0;
	gpi[0].stageCount = 2;
	gpi[0].pStages = guiStages.data();
	gpi[0].pVertexInputState = &vertexInfo;
	gpi[0].pInputAssemblyState = &iaInfo;
	gpi[0].pViewportState = &viewportInfo;
	gpi[0].pRasterizationState = &rasterInfo;
	gpi[0].pMultisampleState = &msInfo;
	gpi[0].pDepthStencilState = &depthInfo;
	gpi[0].pColorBlendState = &blendInfo;
	gpi[0].pDynamicState = &dynState;
	gpi[0].layout = dev.renderData->pipeLayout;
	gpi[0].renderPass = rp_;
	gpi[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

	gpi[1] = gpi[0];
	gpi[1].flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
	gpi[1].basePipelineIndex = 0u;
	gpi[1].pStages = image1DStages.data();

	gpi[2] = gpi[1];
	gpi[2].pStages = image2DStages.data();

	gpi[3] = gpi[1];
	gpi[3].pStages = image3DStages.data();

	VkPipeline pipes[4];
	VK_CHECK(dev.dispatch.CreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, 4, gpi, nullptr, pipes));

	pipes_.gui = pipes[0];
	pipes_.image1D = pipes[1];
	pipes_.image2D = pipes[2];
	pipes_.image3D = pipes[3];

	for(auto& mod : modules) {
		dev.dispatch.DestroyShaderModule(dev.handle, mod, nullptr);
	}

	// init imgui
	this->imgui_ = ImGui::CreateContext();
	ImGui::SetCurrentContext(imgui_);
	this->io_ = &ImGui::GetIO();
	this->io_->IniFilename = nullptr;
	ImGui::GetStyle().WindowRounding = 0.f;
	ImGui::GetStyle().WindowBorderSize = 0.f;
	ImGui::GetStyle().ScrollbarRounding = 0.f;
	// ImGui::GetStyle().FramePadding = {5, 5};
	// ImGui::GetStyle().ItemSpacing = {8, 8};
	// ImGui::GetStyle().ItemInnerSpacing = {6, 6};
	ImGui::GetStyle().Alpha = 0.9f;

	// effectively disable imgui key repeat, we rely on it as input events.
	// ImGui::GetIO().KeyRepeatDelay = 100000000.f;
}

// ~Gui
Gui::~Gui() {
	if(!dev_) {
		return;
	}

	finishDraws();
	draws_.clear();

	if(imgui_) {
		ImGui::DestroyContext(imgui_);
	}

	readbackBuf_.free(dev());

	auto vkDev = dev_->handle;
	if(font_.uploadBuf) dev_->dispatch.DestroyBuffer(vkDev, font_.uploadBuf, nullptr);
	if(font_.uploadMem) dev_->dispatch.FreeMemory(vkDev, font_.uploadMem, nullptr);
	if(font_.view) dev_->dispatch.DestroyImageView(vkDev, font_.view, nullptr);
	if(font_.image) dev_->dispatch.DestroyImage(vkDev, font_.image, nullptr);
	if(font_.mem) dev_->dispatch.FreeMemory(vkDev, font_.mem, nullptr);

	if(pipes_.gui) dev_->dispatch.DestroyPipeline(vkDev, pipes_.gui, nullptr);
	if(pipes_.image1D) dev_->dispatch.DestroyPipeline(vkDev, pipes_.image1D, nullptr);
	if(pipes_.image2D) dev_->dispatch.DestroyPipeline(vkDev, pipes_.image2D, nullptr);
	if(pipes_.image3D) dev_->dispatch.DestroyPipeline(vkDev, pipes_.image3D, nullptr);

	if(rp_) dev_->dispatch.DestroyRenderPass(vkDev, rp_, nullptr);
}

// Renderer
void Gui::ensureFontAtlas(VkCommandBuffer cb) {
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
	VkImageCreateInfo ici {};
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

	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &font_.image));

	VkMemoryRequirements fontImageReq;
	dev.dispatch.GetImageMemoryRequirements(dev.handle, font_.image, &fontImageReq);

	VkMemoryAllocateInfo iai {};
	iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	iai.allocationSize = fontImageReq.size;
	iai.memoryTypeIndex = findLSB(fontImageReq.memoryTypeBits & dev.deviceLocalMemTypeBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &iai, nullptr, &font_.mem));
	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, font_.image, font_.mem, 0));

	// font image view
	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = font_.image;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.levelCount = 1;
	ivi.subresourceRange.layerCount = 1;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &font_.view));

	// Create the upload buffer
	VkBufferCreateInfo bci {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = uploadSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bci, nullptr, &font_.uploadBuf));

	VkMemoryRequirements uploadBufReq;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, font_.uploadBuf, &uploadBufReq);

	VkMemoryAllocateInfo uploadai {};
	uploadai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	uploadai.allocationSize = uploadBufReq.size;
	uploadai.memoryTypeIndex = findLSB(uploadBufReq.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &uploadai, nullptr, &font_.uploadMem));
	VK_CHECK(dev.dispatch.BindBufferMemory(dev.handle, font_.uploadBuf, font_.uploadMem, 0));

	// Upload to Buffer
	char* map = NULL;
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, font_.uploadMem, 0, uploadSize, 0, (void**)(&map)));
	std::memcpy(map, pixels, uploadSize);

	VkMappedMemoryRange range[1] {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = font_.uploadMem;
	range[0].size = uploadSize;
	VK_CHECK(dev.dispatch.FlushMappedMemoryRanges(dev.handle, 1, range));
	dev.dispatch.UnmapMemory(dev.handle, font_.uploadMem);

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
	dev.dispatch.CmdPipelineBarrier(cb,
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
	dev.dispatch.CmdCopyBufferToImage(cb,
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
	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0,
		0, NULL,
		0, NULL,
		1, useBarrier);

	// create descriptor
	VkDescriptorSetAllocateInfo dsai {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = dev.dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &dev_->renderData->dsLayout;
	VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dsai, &dsFont_));

	// ...and update it
	VkDescriptorImageInfo dsii;
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = font_.view;
	dsii.sampler = dev_->renderData->linearSampler;

	VkWriteDescriptorSet write {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = dsFont_;
	write.pImageInfo = &dsii;

	dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);

	// Store our identifier
	font_.drawImage.type = DrawGuiImage::font;
	io.Fonts->TexID = (ImTextureID) &font_.drawImage;
	font_.uploaded = true;
}

void Gui::uploadDraw(Draw& draw, const ImDrawData& drawData) {
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
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, draw.vertexBuffer.mem, 0, vertexSize, 0, (void**) &verts));

	ImDrawIdx* inds;
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, draw.indexBuffer.mem, 0, indexSize, 0, (void**) &inds));

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

	VK_CHECK(dev.dispatch.FlushMappedMemoryRanges(dev.handle, 2, range));
	dev.dispatch.UnmapMemory(dev.handle, draw.vertexBuffer.mem);
	dev.dispatch.UnmapMemory(dev.handle, draw.indexBuffer.mem);
}

void Gui::recordDraw(Draw& draw, VkExtent2D extent, VkFramebuffer fb,
		const ImDrawData& drawData) {
	auto& dev = *dev_;
	if(drawData.TotalIdxCount == 0 && !clear_) {
		return;
	}

	VkRenderPassBeginInfo rpBegin {};
	rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpBegin.renderArea.extent = extent;
	rpBegin.renderPass = rp_;
	rpBegin.framebuffer = fb;

	VkClearValue clearValue;
	if(clear_) {
		clearValue.color = {{0.f, 0.f, 0.f, 1.f}};
		rpBegin.clearValueCount = 1u;
		rpBegin.pClearValues = &clearValue;
	}

	dev.dispatch.CmdBeginRenderPass(draw.cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

	if(drawData.TotalIdxCount > 0) {
		VkViewport viewport {};
		viewport.width = extent.width;
		viewport.height = extent.height;
		viewport.maxDepth = 1.f;
		dev.dispatch.CmdSetViewport(draw.cb, 0, 1, &viewport);

		VkDeviceSize off0 = 0u;
		dev.dispatch.CmdBindVertexBuffers(draw.cb, 0, 1, &draw.vertexBuffer.buf, &off0);
		dev.dispatch.CmdBindIndexBuffer(draw.cb, draw.indexBuffer.buf, 0, VK_INDEX_TYPE_UINT16);

		float pcr[4];
		// scale
		pcr[0] = 2.0f / drawData.DisplaySize.x;
		pcr[1] = 2.0f / drawData.DisplaySize.y;
		// translate
		pcr[2] = -1.0f - drawData.DisplayPos.x * pcr[0];
		pcr[3] = -1.0f - drawData.DisplayPos.y * pcr[1];
		dev.dispatch.CmdPushConstants(draw.cb, dev.renderData->pipeLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcr), pcr);

		auto idxOff = 0u;
		auto vtxOff = 0u;
		for(auto i = 0; i < drawData.CmdListsCount; ++i) {
			auto& cmds = *drawData.CmdLists[i];

			for(auto j = 0; j < cmds.CmdBuffer.Size; ++j) {
				auto& cmd = cmds.CmdBuffer[j];

				VkDescriptorSet ds;
				VkPipeline pipe;
				auto img = (DrawGuiImage*) cmd.TextureId;
				if(img) {
					switch(img->type) {
						case DrawGuiImage::font:
							ds = dsFont_;
							pipe = pipes_.gui;
							break;
						case DrawGuiImage::e1d:
							ds = draw.dsSelected;
							pipe = pipes_.image1D;
							break;
						case DrawGuiImage::e2d:
							ds = draw.dsSelected;
							pipe = pipes_.image2D;
							break;
						case DrawGuiImage::e3d:
							ds = draw.dsSelected;
							pipe = pipes_.image3D;
							break;
						default:
							dlg_error("Invalid DrawGuiImage");
							pipe = {};
							ds =  {};
							break;
					}

					if(img->type != DrawGuiImage::font) {
						// bind push constant data
						struct PcrImageData {
							float layer;
							float valMin;
							float valMax;
							u32 flags;
						} pcr = {
							img->layer,
							img->minValue,
							img->maxValue,
							img->flags
						};

						dev.dispatch.CmdPushConstants(draw.cb, dev.renderData->pipeLayout,
							VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 16,
							sizeof(pcr), &pcr);
					}
				} else {
					// default: drawing imgui stuff.
					// Use gui pipe and font texture
					ds = dsFont_;
					pipe = pipes_.gui;
				}

				dev.dispatch.CmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				dev.dispatch.CmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					dev.renderData->pipeLayout, 0, 1, &ds, 0, nullptr);

				VkRect2D scissor {};
				scissor.offset.x = std::max<int>(cmd.ClipRect.x - drawData.DisplayPos.x, 0);
				scissor.offset.y = std::max<int>(cmd.ClipRect.y - drawData.DisplayPos.y, 0);
				scissor.extent.width = cmd.ClipRect.z - cmd.ClipRect.x;
				scissor.extent.height = cmd.ClipRect.w - cmd.ClipRect.y;
				dev.dispatch.CmdSetScissor(draw.cb, 0, 1, &scissor);

				dev.dispatch.CmdDrawIndexed(draw.cb, cmd.ElemCount, 1, idxOff, vtxOff, 0);
				idxOff += cmd.ElemCount;
			}

			vtxOff += cmds.VtxBuffer.Size;
		}
	}

	dev.dispatch.CmdEndRenderPass(draw.cb);
}


void Gui::drawOverviewUI(Draw& draw) {
	(void) draw;

	auto& dev = *this->dev_;

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
	ImGui::Text("Physical device, API version");
	ImGui::Text("Driver version");

	ImGui::NextColumn();

	ImGui::Text("%s %d.%d.%d", dev.props.deviceName,
		VK_VERSION_MAJOR(dev.props.apiVersion),
		VK_VERSION_MINOR(dev.props.apiVersion),
		VK_VERSION_PATCH(dev.props.apiVersion));
	ImGui::Text("%d.%d.%d",
		VK_VERSION_MAJOR(dev.props.driverVersion),
		VK_VERSION_MINOR(dev.props.driverVersion),
		VK_VERSION_PATCH(dev.props.driverVersion));

	ImGui::Columns();

	// pretty much just own debug stuff
	ImGui::Separator();

	auto pending = dlg::format("Pending submissions: {}", dev.pending.size());
	if(ImGui::TreeNode(&dev.pending, "%s", pending.c_str())) {
		for(auto& subm : dev.pending) {
			// TODO: make button?
			imGuiText("To queue {}", name(*subm->queue));
			if(subm->appFence) {
				imGuiText("Using Fence");
				if(ImGui::Button(name(*subm->appFence).c_str())) {
					selectResource(*subm->appFence);
				}
			}

			ImGui::Indent();
			for(auto& sub : subm->submissions) {
				// TODO: show semaphores
				// should probably store Semaphore instead of VkSemaphore!
				// if(!sub.waitSemaphores.empty()) {
				// 	for(auto [sem, flag] : sub.waitSemaphores) {
				// 	}
				// }

				for(auto* cb : sub.cbs) {
					ImGui::Bullet();
					// We have the additional IsItemClicked here since
					// that might change every frame
					if(ImGui::Button(name(*cb).c_str()) || ImGui::IsItemClicked()) {
						selectCb(*cb);
					}
				}
			}
			ImGui::Unindent();

			// ImGui::Separator();
		}
		ImGui::TreePop();
	}

	ImGui::Columns();
}

void Gui::draw(Draw& draw, bool fullscreen) {
	ImGui::NewFrame();

	unsigned flags = 0u;
	if(fullscreen) {
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		flags = ImGuiWindowFlags_NoDecoration;
	} else {
		// ImGui::ShowDemoWindow();
		// ImGui::ShowAboutWindow();
		// ImGui::ShowMetricsWindow();
		ImGui::SetNextWindowPos({80, 80}, ImGuiCond_Once);
		ImGui::SetNextWindowSize({800, 500}, ImGuiCond_Once);
		// auto flags = 0;
	}

	auto checkSelectTab = [&](Tab tab) {
		auto flags = 0;
		if(activeTab_ == tab && activateTabCounter_ < 2) {
			flags = ImGuiTabItemFlags_SetSelected;
			++activateTabCounter_;
		}

		return flags;
	};

	if(ImGui::Begin("Fuencaliente", nullptr, flags)) {
		if(ImGui::BeginTabBar("MainTabBar")) {
			if(ImGui::BeginTabItem("Overview")) {
				drawOverviewUI(draw);
				ImGui::EndTabItem();
			}

			if(ImGui::BeginTabItem("Resources", nullptr, checkSelectTab(Tab::resources))) {
				tabs_.resources.draw(draw);
				ImGui::EndTabItem();
			}

			if(tabs_.cb.cb_) {
				if(ImGui::BeginTabItem("Command Buffer", nullptr, checkSelectTab(Tab::commandBuffer))) {
					tabs_.cb.draw();
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

void Gui::destroyed(const Handle& handle) {
	tabs_.resources.destroyed(handle);
	tabs_.cb.destroyed(handle);

	// Make sure that all our submissions that use the given handle have
	// finished.
	std::vector<VkFence> fences;
	std::vector<Draw*> draws;
	for(auto& draw : draws_) {
		if(!draw.inUse) {
			continue;
		}

		auto it = find(draw.usedHandles, &handle);
		if(it != draw.usedHandles.end()) {
			fences.push_back(draw.fence);
			draws.push_back(&draw);
		}
	}

	if(fences.empty()) {
		return;
	}

	VK_CHECK(dev().dispatch.WaitForFences(dev().handle, u32(fences.size()),
		fences.data(), true, UINT64_MAX));
	VK_CHECK(dev().dispatch.ResetFences(dev().handle, u32(fences.size()), fences.data()));

	for(auto* draw : draws) {
		draw->usedHandles.clear();
		draw->inUse = false;
	}
}

void Gui::activateTab(Tab tab) {
	activeTab_ = tab;
	activateTabCounter_ = 0u;
}

// TODO: atm, we always call this while device mutex is locked.
// When waiting for fences, it might block application progress *a lot*,
// we really need to unlock it while waiting for fences (or otherwise
// blocking ourselves!)
template<typename H>
void Gui::waitForSubmissions(const H& handle) {
	// find all submissions associated with this image
	std::vector<PendingSubmission*> toComplete;

	// We can implement this in two possible way: either check
	// for all command buffers using the handle whether they are
	// pending or check for all pending submissions whether they
	// use the handle.
	for(auto it = dev().pending.begin(); it != dev().pending.end();) {
		auto& pending = *it;

		// remove finished pending submissions.
		// important to do this before accessing them.
		if(auto nit = checkLocked(*pending); nit) {
			// don't increase iterator as the current one
			// was erased.
			it = *nit;
			continue;
		}

		bool wait = false;
		for(auto& sub : pending->submissions) {
			for(auto* cb : sub.cbs) {
				dlg_assert(cb->state == CommandBuffer::State::executable);
				if(!cb->uses(handle)) {
					continue;
				}

				wait = true;
				break;
			}
		}

		if(wait) {
			toComplete.push_back(pending.get());
		}

		++it;
	}

	waitFor(toComplete);
}

void Gui::waitFor(span<PendingSubmission*> toComplete) {
	if(toComplete.empty()) {
		return;
	}

	std::vector<VkFence> fences;
	for(auto* pending : toComplete) {
		if(pending->appFence) {
			fences.push_back(pending->appFence->handle);
		} else {
			fences.push_back(pending->ourFence);
		}
	}

	VK_CHECK(dev().dispatch.WaitForFences(dev().handle,
		u32(fences.size()), fences.data(), true, UINT64_MAX));

	for(auto* pending : toComplete) {
		auto res = checkLocked(*pending);
		// we waited for it above. It should really
		// be completed now.
		dlg_assert(res);
	}
}

Gui::FrameResult Gui::renderFrame(FrameInfo& info) {
	// find a free draw objectg
	Draw* foundDraw = nullptr;
	for(auto& draw : draws_) {
		if(!draw.inUse) {
			foundDraw = &draw;
			break;
		}

		if(dev().dispatch.GetFenceStatus(dev().handle, draw.fence) == VK_SUCCESS) {
			VK_CHECK(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence));
			foundDraw = &draw;
			break;
		}
	}

	if(!foundDraw) {
		foundDraw = &draws_.emplace_back();
		foundDraw->init(dev());
	}

	auto& draw = *foundDraw;
	draw.usedHandles.clear();

	VkCommandBufferBeginInfo cbBegin {};
	cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(dev().dispatch.BeginCommandBuffer(draw.cb, &cbBegin));

	ensureFontAtlas(draw.cb);

	makeImGuiCurrent();
	ImGui::GetIO().DisplaySize.x = info.extent.width;
	ImGui::GetIO().DisplaySize.y = info.extent.height;

	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;
	auto now = Clock::now();
	auto diff = now - lastFrame_;
	lastFrame_ = now;
	auto dt = std::chrono::duration_cast<Secf>(diff).count();
	if(dt > 0.f) {
		ImGui::GetIO().DeltaTime = dt;
	}

	{
		// Important we already lock this mutex here since we need to make
		// sure no new submissions are done by application while we process
		// and evaluate the pending submissions
		std::lock_guard queueLock(dev().queueMutex);

		// TODO: device mutex should probably be unlocked for our
		// queue calls in the end. Care must be taken though!
		std::lock_guard devMutex(dev().mutex);

		// handle buffer waiting/retrieving logic
		auto pSelBuf = std::get_if<Buffer*>(&tabs_.resources.handle_);
		auto* selBuf = pSelBuf ? *pSelBuf : nullptr;

		if(selBuf) {
			// TODO: ugh, all of this is so expensive.
			// Here, we could definitely try out semaphore chaining since we only
			// need the easy way (app subm -> our subm)
			// TODO: we probably don't wanna wait for this. Instead, retrive
			// the written contents from the last finished draw (if it
			// was for the same buffer).
			waitForSubmissions(*selBuf);

			// TODO: offset, sizes and stuff
			auto size = std::min(selBuf->ci.size, VkDeviceSize(1024 * 16));
			readbackBuf_.ensure(dev(), size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

			// TODO: support srcOffset
			// TODO: need memory barriers
			VkBufferCopy copy {};
			copy.size = size;
			dev().dispatch.CmdCopyBuffer(draw.cb, selBuf->handle, readbackBuf_.buf,
				1, &copy);
			dev().dispatch.EndCommandBuffer(draw.cb);

			VkSubmitInfo submitInfo {};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submitInfo.commandBufferCount = 1u;
			submitInfo.pCommandBuffers = &draw.cb;

			auto res = dev().dispatch.QueueSubmit(dev().gfxQueue->handle, 1u, &submitInfo, draw.fence);
			if(res != VK_SUCCESS) {
				dlg_error("vkSubmit error: {}", vk::name(res));
				return {res, &draw};
			}

			// TODO: ouch! this is really expensive.
			// could we at least release the device mutex lock meanwhile?
			// we would have to check for various things tho, if resource
			// was destroyed.
			VK_CHECK(dev().dispatch.WaitForFences(dev().handle, 1, &draw.fence, true, UINT64_MAX));
			VK_CHECK(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence));

			void* mapped {};
			VK_CHECK(dev().dispatch.MapMemory(dev().handle, readbackBuf_.mem, 0, size, {}, &mapped));

			VkMappedMemoryRange range {};
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = readbackBuf_.mem;
			range.offset = 0u;
			range.size = size;
			VK_CHECK(dev().dispatch.InvalidateMappedMemoryRanges(dev().handle, 1, &range));

			tabs_.resources.buffer_.lastRead.resize(size);
			std::memcpy(tabs_.resources.buffer_.lastRead.data(), mapped, size);

			dev().dispatch.UnmapMemory(dev().handle, readbackBuf_.mem);

			VkCommandBufferBeginInfo cbBegin {};
			cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			VK_CHECK(dev().dispatch.BeginCommandBuffer(draw.cb, &cbBegin));
		}

		// handle cb waiting logic:
		// if we have hooked a command buffer to retrieve its timing, we have
		// to wait for it to complete.
		// TODO: could be optimized I guess, we might not strictly have to do this
		// with some smart query pool management.
		if(tabs_.cb.cb_ && tabs_.cb.hooked_.cb) {
			// NOTE: it's important we do a copy here since waiting for the
			// submissions might modify cb_->pending itself, invalidating
			// the span.
			auto pendingCopy = tabs_.cb.cb_->pending;
			waitFor(pendingCopy);
		}

		this->draw(draw, info.fullscreen);
		auto& drawData = *ImGui::GetDrawData();
		this->uploadDraw(draw, drawData);

		// handle image waiting logic:
		// if we are displaying an image we have to make sure it is not currently
		// being written somewhere else.
		auto pSelImg = std::get_if<Image*>(&tabs_.resources.handle_);
		auto* selImg = pSelImg ? *pSelImg : nullptr;
		if(selImg && !tabs_.resources.image_.view) {
			selImg = nullptr;
		}

		VkImageLayout finalLayout {};
		if(selImg) {
			auto& img = *selImg;
			finalLayout = img.pendingLayout;
			waitForSubmissions(img);

			// Make sure our image is in the right layout.
			// And we are allowed to read it
			VkImageMemoryBarrier imgb {};
			imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imgb.image = img.handle;
			imgb.subresourceRange = tabs_.resources.image_.subres;
			imgb.oldLayout = finalLayout;
			imgb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imgb.srcAccessMask = {}; // TODO: dunno. Track/figure out possible flags?
			imgb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			// TODO: transfer queue.
			// We currently just force concurrent mode on image/buffer creation
			// but that might have performance impact.
			// Requires additional submissions to the other queues.
			// We should first check whether the queue is different in first place.
			// if(img.ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
			// }

			dev().dispatch.CmdPipelineBarrier(draw.cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // wait for everything
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
				0, 0, nullptr, 0, nullptr, 1, &imgb);

			draw.usedHandles.push_back(selImg);
		}

		this->recordDraw(draw, info.extent, info.fb, drawData);

		if(selImg) {
			// return it to original layout
			VkImageMemoryBarrier imgb {};
			imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imgb.image = selImg->handle;
			imgb.subresourceRange = tabs_.resources.image_.subres;
			imgb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imgb.newLayout = finalLayout;
			imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			dlg_assert(
				finalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
				finalLayout != VK_IMAGE_LAYOUT_UNDEFINED);
			imgb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			imgb.srcAccessMask = {}; // TODO: dunno. Track/figure out possible flags

			// TODO: transfer queue.
			// We currently just force concurrent mode on image/buffer creation
			// but that might have performance impact.
			// Requires additional submissions to the other queues.
			// We should first check whether the queue is different in first place.
			// if(selImg->ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
			// }

			dev().dispatch.CmdPipelineBarrier(draw.cb,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // wait in everything
				0, 0, nullptr, 0, nullptr, 1, &imgb);
		}

		dev().dispatch.EndCommandBuffer(draw.cb);

		// submit batch
		// TODO: clean this up.
		// handle case where application doesn't give us semaphore
		// (and different queues are used?)
		auto waitStages = std::make_unique<VkPipelineStageFlags[]>(info.waitSemaphores.size());
		for(auto i = 0u; i < info.waitSemaphores.size(); ++i) {
			waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		}

		// TODO: we could add dev.resetSemaphores here as wait semaphores.
		// And move their vector into Draw. And when the draw fence is ready,
		// move them back into the semaphore pool.

		VkSubmitInfo submitInfo {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1u;
		submitInfo.pCommandBuffers = &draw.cb;
		submitInfo.signalSemaphoreCount = 1u;
		submitInfo.pSignalSemaphores = &draw.semaphore;
		submitInfo.pWaitDstStageMask = waitStages.get();
		submitInfo.pWaitSemaphores = info.waitSemaphores.data();
		submitInfo.waitSemaphoreCount = u32(info.waitSemaphores.size());

		auto res = dev().dispatch.QueueSubmit(dev().gfxQueue->handle, 1u, &submitInfo, draw.fence);
		if(res != VK_SUCCESS) {
			dlg_error("vkSubmit error: {}", vk::name(res));
			return {res, &draw};
		}
	}

	// call down
	VkPresentInfoKHR presentInfo {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pImageIndices = &info.imageIdx;
	presentInfo.pWaitSemaphores = &draw.semaphore;
	presentInfo.waitSemaphoreCount = 1u;
	presentInfo.pSwapchains = &info.swapchain;
	presentInfo.swapchainCount = 1u;
	// TODO: might be bad to not forward this pi.pNext (for overlay)

	auto res = dev().dispatch.QueuePresentKHR(info.presentQueue, &presentInfo);
	if(res != VK_SUCCESS) {
		dlg_error("vkQueuePresentKHR error: {}", vk::name(res));

		VK_CHECK(dev().dispatch.WaitForFences(dev().handle, 1, &draw.fence, true, UINT64_MAX));
		VK_CHECK(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence));

		draw.usedHandles.clear();
		draw.inUse = false;

		return {res, &draw};
	}

	foundDraw->inUse = true;

	// TODO: do we have to lock the mutex while waiting?
	// TODO: support not doing this.
	// For that we would have to chain future submissions using resources
	// used by this draw via semaphores...
	// TODO: hacky hacky
	// TODO: code duplication with above. Should probably be moved to
	// extra function
	if(!draw.usedHandles.empty()) {
		// dlg_trace("awaiting frame");

		VK_CHECK(dev().dispatch.WaitForFences(dev().handle, 1, &draw.fence, true, UINT64_MAX));
		VK_CHECK(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence));

		draw.usedHandles.clear();
		draw.inUse = false;
	}

	return {VK_SUCCESS, &draw};
}

void Gui::finishDraws() {
	std::vector<VkFence> fences;
	for(auto& draw : draws_) {
		if(draw.inUse) {
			fences.push_back(draw.fence);
		}
	}

	if(!fences.empty()) {
		VK_CHECK(dev().dispatch.WaitForFences(dev().handle, u32(fences.size()), fences.data(), true, UINT64_MAX));
		VK_CHECK(dev().dispatch.ResetFences(dev().handle, u32(fences.size()), fences.data()));

		for(auto& draw : draws_) {
			draw.inUse = false;
			draw.usedHandles.clear();
		}
	}
}

void Gui::makeImGuiCurrent() {
	ImGui::SetCurrentContext(imgui_);
}

} // namespace fuen
