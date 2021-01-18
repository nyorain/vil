#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/render.hpp>
#include <gui/commandHook.hpp>
#include <layer.hpp>
#include <queue.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <handles.hpp>
#include <commands.hpp>
#include <util/util.hpp>
#include <util/bytes.hpp>

#include <spirv_reflect.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>

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

	// TODO: likely needs a lock. Shouldn't be done here in first place
	// i guess but at Gui creation, where an existing gui object could
	// be moved.
	dlg_assert(dev.gui == nullptr);
	dev.gui = this;

	tabs_.cb.gui_ = this;
	tabs_.resources.gui_ = this;

	lastFrame_ = Clock::now();

	// init command pool
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = dev.gfxQueue->family;
	VK_CHECK(dev.dispatch.CreateCommandPool(dev.handle, &cpci, nullptr, &commandPool_));
	nameHandle(dev, commandPool_, "Gui:commandPool");

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
	nameHandle(dev, rp_, "Gui:rp");

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

	nameHandle(dev, pipes_.gui, "Gui:pipeGui");
	nameHandle(dev, pipes_.image1D, "Gui:pipeImage1D");
	nameHandle(dev, pipes_.image2D, "Gui:pipeImage2D");
	nameHandle(dev, pipes_.image3D, "Gui:pipeImage3D");

	for(auto& mod : modules) {
		dev.dispatch.DestroyShaderModule(dev.handle, mod, nullptr);
	}

	// init imgui
	this->imgui_ = ImGui::CreateContext();
	ImGui::SetCurrentContext(imgui_);
	this->io_ = &ImGui::GetIO();
	this->io_->IniFilename = nullptr;
	this->io_->MouseDrawCursor = false;
	ImGui::GetStyle().WindowRounding = 0.f;
	ImGui::GetStyle().WindowBorderSize = 0.f;
	ImGui::GetStyle().ScrollbarRounding = 0.f;
	// ImGui::GetStyle().FramePadding = {5, 5};
	// ImGui::GetStyle().ItemSpacing = {8, 8};
	// ImGui::GetStyle().ItemInnerSpacing = {6, 6};
	// ImGui::GetStyle().Alpha = 0.9f;

	// effectively disable imgui key repeat, we rely on it as input events.
	// ImGui::GetIO().KeyRepeatDelay = 100000000.f;
}

// ~Gui
Gui::~Gui() {
	if(!dev_) {
		return;
	}

	// TODO: needs a lock. Likely also shouldn't be here.
	dlg_assert(dev_->gui == this);
	dev_->gui = nullptr;

	finishDraws();
	draws_.clear();

	if(imgui_) {
		ImGui::DestroyContext(imgui_);
	}

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
	if(commandPool_) dev_->dispatch.DestroyCommandPool(vkDev, commandPool_, nullptr);
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

	// Clear pending submissions
	for(auto it = dev.pending.begin(); it != dev.pending.end();) {
		auto& subm = *it;
		if(auto nit = checkLocked(*subm); nit) {
			it = *nit;
			continue;
		}

		++it; // already increment to next one so we can't miss it
	}

	auto pending = dlg::format("Pending submissions: {}", dev.pending.size());
	if(ImGui::TreeNode(&dev.pending, "%s", pending.c_str())) {
		for(auto& subm : dev.pending) {
			// TODO: make button
			imGuiText("To queue {}", name(*subm->queue));
			if(subm->appFence) {
				imGuiText("Using Fence");
				if(ImGui::Button(name(*subm->appFence).c_str())) {
					selectResource(*subm->appFence);
				}
			}

			ImGui::Indent();
			for(auto& sub : subm->submissions) {
				// TODO: show semaphores?
				// should probably store Semaphore instead of VkSemaphore!
				// if(!sub.waitSemaphores.empty()) {
				// 	for(auto [sem, flag] : sub.waitSemaphores) {
				// 	}
				// }

				for(auto& [cb, _] : sub.cbs) {
					ImGui::Bullet();
					// We have the additional IsItemClicked here since
					// that might change every frame
					// if(ImGui::Button(name(*cb).c_str()) || ImGui::IsItemClicked()) {
					// 	selectCb(*cb);
					// }

					auto* group = cb->lastRecordLocked()->group;
					dlg_assert(group);

					auto label = dlg::format("Group {}", (void*) group);
					if(ImGui::Button(label.c_str())) {
						tabs_.cb.select(group->lastRecord, true);
						this->activateTab(Tab::commandBuffer);
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
	resourcesTabDrawn_ = false;

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
				resourcesTabDrawn_ = true;
			}

			// if(tabs_.cb.cb_) {
			if(tabs_.cb.record_) {
				if(ImGui::BeginTabItem("Command Buffer", nullptr, checkSelectTab(Tab::commandBuffer))) {
					tabs_.cb.draw(draw);
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

	// TODO: would have to lock mutex
	// for(auto* draw : draws) {
	// 	finished(*draw);
	// }
}

void Gui::activateTab(Tab tab) {
	activeTab_ = tab;
	activateTabCounter_ = 0u;
}

Gui::FrameResult Gui::renderFrame(FrameInfo& info) {
	makeImGuiCurrent();

	// find a free draw objectg
	Draw* foundDraw = nullptr;
	for(auto& draw : draws_) {
		if(!draw.inUse) {
			foundDraw = &draw;
			break;
		}

		if(dev().dispatch.GetFenceStatus(dev().handle, draw.fence) == VK_SUCCESS) {
			finished(draw);
			foundDraw = &draw;
			break;
		}
	}

	if(!foundDraw) {
		foundDraw = &draws_.emplace_back();
		foundDraw->init(dev(), commandPool_);
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

	// TODO: hacky but we have to keep the record alive, making sure
	// it's not destroyed inside the lock.
	auto keepAlive = tabs_.cb.record_;

	{
		// Important we already lock this mutex here since we need to make
		// sure no new submissions are done by application while we process
		// and evaluate the pending submissions
		std::lock_guard queueLock(dev().queueMutex);
		std::lock_guard devMutex(dev().mutex);

		this->draw(draw, info.fullscreen);
		auto& drawData = *ImGui::GetDrawData();
		this->uploadDraw(draw, drawData);

		// General barrier to make sure all past submissions writing resources
		// we read are done. Not needed when we don't read a device resource.
		// TODO could likely at least give a better srcAccessMask
		if(!draw.usedHandles.empty()) {
			VkMemoryBarrier memb {};
			memb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			memb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

			dev().dispatch.CmdPipelineBarrier(draw.cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				0, 1, &memb, 0, nullptr, 0, nullptr);
		}

		if(resourcesTabDrawn_) {
			tabs_.resources.recordPreRender(draw);
		}

		this->recordDraw(draw, info.extent, info.fb, drawData);

		if(resourcesTabDrawn_) {
			tabs_.resources.recoredPostRender(draw);
		}

		// General barrier to make sure all our reading is done before
		// future application submissions to this queue.
		// Not needed when we don't read a device resource.
		// TODO could likely at least give a better srcAccessMask
		if(!draw.usedHandles.empty()) {
			VkMemoryBarrier memb {};
			memb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			memb.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;

			dev().dispatch.CmdPipelineBarrier(draw.cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				0, 1, &memb, 0, nullptr, 0, nullptr);
		}

		dev().dispatch.EndCommandBuffer(draw.cb);

		// submit batch
		VkSubmitInfo submitInfo {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1u;
		submitInfo.pCommandBuffers = &draw.cb;

		std::unordered_set<Submission*> waitSubmissions;

		// NOTE: could alternatively retrieve all submissions via
		// handle->refRecords->cb->pending (and handle->descriptos->...)
		// but this should be faster, there are usually only a small
		// number of pending submissions while there might be more recordings
		// referencing a handle.

		for(auto& pending : dev().pending) {
			auto subs = needsSyncLocked(*pending, draw);
			waitSubmissions.insert(subs.begin(), subs.end());
		}

		u64 signalValues[2];
		std::vector<u64> waitValues;
		VkTimelineSemaphoreSubmitInfo tsInfo {};

		auto waitSems = std::vector(info.waitSemaphores.begin(), info.waitSemaphores.end());

		std::vector<VkPipelineStageFlags> waitStages;
		for(auto i = 0u; i < info.waitSemaphores.size(); ++i) {
			// TODO: we might be able to do better
			waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}

		if(dev().timelineSemaphores) {
			dlg_assert(dev().resetSemaphores.empty());

			tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
			tsInfo.pNext = submitInfo.pNext;

			// wait
			waitValues.resize(waitSems.size());
			for(auto* sub : waitSubmissions) {
				waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); // TODO: guess we could do better
				waitSems.push_back(sub->ourSemaphore);
				waitValues.push_back(sub->ourSemaphoreValue);

				draw.waitedUpon.push_back(sub->ourSemaphore);
			}

			// signal
			// signalValues[0] is uninitialized by design, should be
			// ignored by driver as signalSemaphores[0] is binary
			VK_CHECK(dev().dispatch.GetSemaphoreCounterValue(dev().handle,
				draw.futureSemaphore, &signalValues[1]));
			++signalValues[1];
			draw.futureSemaphoreValue = signalValues[1];

			tsInfo.signalSemaphoreValueCount = 2u;
			tsInfo.pSignalSemaphoreValues = signalValues;

			submitInfo.pNext = &tsInfo;
		} else {
			// add dev.resetSemaphores while we are at it
			for(auto sem : dev().resetSemaphores) {
				waitSems.push_back(sem);
				waitStages.push_back(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
				draw.waitedUpon.push_back(sem);
			}

			dev().resetSemaphores.clear();

			// if we have to wait for one submission, we wait for all,
			// for simplicity's sake. NOTE: could be improved for a potential
			// performance gain but JUST UPDATE YOUR DRIVERS AND GET
			// TIMELINE SEMAPHORES ffs.
			// TODO, more reasonable improvement: instead of ever
			// waiting on cpu side, simply insert new semaphores to
			// respective queues, where needed.
			auto waitCpu = false;
			for(auto* sub : waitSubmissions) {
				if(!sub->ourSemaphore) {
					waitCpu = true;
					break;
				}
			}

			if(waitCpu) {
				std::unordered_set<PendingSubmission*> subs;
				for(auto* sub : waitSubmissions) {
					subs.insert(sub->parent);
				}

				std::vector<VkFence> fences;
				for(auto* pending : subs) {
					if(pending->appFence) {
						fences.push_back(pending->appFence->handle);
					} else {
						fences.push_back(pending->ourFence);
					}
				}

				// TODO: waiting inside lock :(
				VK_CHECK(dev().dispatch.WaitForFences(dev().handle,
					u32(fences.size()), fences.data(), true, UINT64_MAX));

				for(auto* pending : subs) {
					auto res = checkLocked(*pending);
					dlg_assert(res);
				}
			} else {
				for(auto* sub : waitSubmissions) {
					waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); // TODO: guess we could do better
					waitSems.push_back(sub->ourSemaphore);

					draw.waitedUpon.push_back(sub->ourSemaphore);
				}
			}
		}

		dlg_assert(waitStages.size() == waitSems.size());

		auto signalSems = std::array{draw.presentSemaphore, draw.futureSemaphore};
		submitInfo.signalSemaphoreCount = u32(signalSems.size());
		submitInfo.pSignalSemaphores = signalSems.data();
		submitInfo.pWaitDstStageMask = waitStages.data();
		submitInfo.waitSemaphoreCount = u32(waitSems.size());
		submitInfo.pWaitSemaphores = waitSems.data();

		auto res = dev().dispatch.QueueSubmit(dev().gfxQueue->handle, 1u, &submitInfo, draw.fence);
		if(res != VK_SUCCESS) {
			dlg_error("vkSubmit error: {}", vk::name(res));
			return {res, &draw};
		}
	}

	draw.inUse = true;

	// call down
	VkPresentInfoKHR presentInfo {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pImageIndices = &info.imageIdx;
	presentInfo.pWaitSemaphores = &draw.presentSemaphore;
	presentInfo.waitSemaphoreCount = 1u;
	presentInfo.pSwapchains = &info.swapchain;
	presentInfo.swapchainCount = 1u;
	// TODO: forward pNext for all extensions we know. Really important
	// here, might contain important information.
	// Maybe just forward everything? Warn for unknown types?

	{
		std::lock_guard queueLock(dev().queueMutex);

		auto res = dev().dispatch.QueuePresentKHR(info.presentQueue, &presentInfo);
		if(res != VK_SUCCESS) {
			dlg_error("vkQueuePresentKHR error: {}", vk::name(res));

			// TODO: not sure how to handle this the best
			VK_CHECK(dev().dispatch.WaitForFences(dev().handle, 1, &draw.fence, true, UINT64_MAX));
			finished(draw);

			return {res, nullptr};
		}
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
		for(auto& draw : draws_) {
			finished(draw);
		}
	}
}

void Gui::makeImGuiCurrent() {
	ImGui::SetCurrentContext(imgui_);
}

void Gui::selectCommands(IntrusivePtr<CommandRecord> record,
		bool updateFromGroup, bool activateTab) {
	tabs_.cb.select(record, updateFromGroup);
	if(activateTab) {
		this->activateTab(Tab::commandBuffer);
	}
}

void Gui::selectResource(Handle& handle, bool activateTab) {
	tabs_.resources.select(handle);
	if(activateTab) {
		this->activateTab(Tab::resources);
	}
}

std::vector<Draw*> Gui::pendingDraws() {
	std::vector<Draw*> ret;
	for(auto& draw : draws_) {
		if(draw.inUse) {
			ret.push_back(&draw);
		}
	}

	return ret;
}

void Gui::finished(Draw& draw) {
	dlg_assert(draw.inUse);
	dlg_assert(dev().dispatch.GetFenceStatus(dev().handle, draw.fence) == VK_SUCCESS);

	{
		std::lock_guard lock(dev().mutex);
		for(auto semaphore : draw.waitedUpon) {
			dev().semaphorePool.push_back(semaphore);
		}
	}

	auto& rb = tabs_.resources.buffer_;
	// NOTE: handle case where offset isn't the same? could patch-in
	// relevant data nonetheless
	if(rb.handle &&
			rb.handle->handle == draw.readback.src &&
			rb.offset == draw.readback.offset) {
		ensureSize(rb.lastRead, draw.readback.size);

		VkMappedMemoryRange range {};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = draw.readback.copy.mem;
		range.size = VK_WHOLE_SIZE;
		range.offset = 0u;
		VK_CHECK(dev().dispatch.FlushMappedMemoryRanges(dev().handle, 0, &range));

		// TODO(perf): with some clever syncing we could probably get around
		// the copy here and instead read from the mapped memory directly.
		auto src = static_cast<std::byte*>(draw.readback.map);
		std::copy(src, src + draw.readback.size, rb.lastRead.begin());
	}

	draw.waitedUpon.clear();
	draw.usedHandles.clear();
	draw.usedHookState.reset();

	VK_CHECK(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence));

	draw.inUse = false;
	draw.futureSemaphoreUsed = 0u;
	draw.futureSemaphoreValue = 0u;
}

// util
void refButton(Gui& gui, Handle& handle) {
	if(ImGui::Button(name(handle).c_str())) {
		gui.selectResource(handle);
	}
}

void refButtonOpt(Gui& gui, Handle* handle) {
	if(handle) {
		refButton(gui, *handle);
	}
}

void refButtonExpect(Gui& gui, Handle* handle) {
	dlg_assert(handle);
	if(handle) {
		refButton(gui, *handle);
	}
}

void refButtonD(Gui& gui, Handle* handle, const char* str) {
	if(handle) {
		refButton(gui, *handle);
	} else {
		ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);

		// NOTE: could add popup to button further explaining what's going on
		ImGui::Button(str);

		ImGui::PopStyleVar();
		ImGui::PopItemFlag();
	}
}

} // namespace fuen
