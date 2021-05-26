#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/render.hpp>
#include <gui/commandHook.hpp>
#include <gui/arimo.hpp>
#include <layer.hpp>
#include <queue.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <handles.hpp>
#include <command/commands.hpp>
#include <util/util.hpp>
#include <util/bytes.hpp>
#include <util/vecOps.hpp>
#include <util/profiling.hpp>

#include <swa/key.h>
#include <util/spirv_reflect.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>

#include <set>
#include <map>
#include <fstream>
#include <filesystem>

#include <gui.frag.spv.h>
#include <gui.vert.spv.h>

#include <image.frag.1DArray.spv.h>
#include <image.frag.u1DArray.spv.h>
#include <image.frag.i1DArray.spv.h>

#include <image.frag.2DArray.spv.h>
#include <image.frag.u2DArray.spv.h>
#include <image.frag.i2DArray.spv.h>

#include <image.frag.3D.spv.h>
#include <image.frag.u3D.spv.h>
#include <image.frag.i3D.spv.h>

inline namespace imgui_vil {

thread_local ImGuiContext* __LayerImGui;

}

namespace vil {

// Gui
void Gui::init(Device& dev, VkFormat colorFormat, VkFormat depthFormat, bool clear) {
	dev_ = &dev;
	clear_ = clear;

	// TODO: likely needs a lock. Shouldn't be done here in first place
	// i guess but at Gui creation, where an existing gui object could
	// be moved.
	dlg_assert(dev.gui == nullptr);
	dev.gui = this;

	lastFrame_ = Clock::now();

	auto blurBackground = true;
	if(!clear && blurBackground) {
		vil::init(blur_, dev, dev.renderData->linearSampler);

		VkDescriptorSetAllocateInfo dai {};
		dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dai.descriptorSetCount = 1u;
		dai.pSetLayouts = &dev.renderData->dsLayout;
		dai.descriptorPool = dev.dsPool;
		VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dai, &blurDs_));
		nameHandle(dev, blurDs_, "Gui:blurDs");
	}

	// init command pool
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = dev.gfxQueue->family;
	VK_CHECK(dev.dispatch.CreateCommandPool(dev.handle, &cpci, nullptr, &commandPool_));
	nameHandle(dev, commandPool_, "Gui:commandPool");

	// init render stuff
	VkAttachmentDescription atts[2] {};

	auto& colorAtt = atts[0];
	colorAtt.format = colorFormat;
	colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAtt.loadOp = clear ?
		VK_ATTACHMENT_LOAD_OP_CLEAR :
		VK_ATTACHMENT_LOAD_OP_LOAD;
	colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAtt.initialLayout = clear ?
		VK_IMAGE_LAYOUT_UNDEFINED :
		VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	auto& depthAtt = atts[1];
	depthAtt.format = depthFormat;
	depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; // don't really care atm

	VkAttachmentReference colorRef = {};
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthRef = {};
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	VkRenderPassCreateInfo rpi {};
	rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpi.attachmentCount = 2;
	rpi.pAttachments = atts;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;

	VK_CHECK(dev.dispatch.CreateRenderPass(dev.handle, &rpi, nullptr, &rp_));
	nameHandle(dev, rp_, "Gui:rp");

	// pipeline
	VkShaderModule vertModule;
	VkShaderModuleCreateInfo vertShaderInfo {};
	vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertShaderInfo.codeSize = sizeof(gui_vert_spv_data);
	vertShaderInfo.pCode = gui_vert_spv_data;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &vertShaderInfo, NULL, &vertModule));

	std::vector<VkShaderModule> modules;
	auto initStages = [&](span<const u32> fragSpv) {
		VkShaderModule fragModule;
		VkShaderModuleCreateInfo fragShaderInfo {};
		fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		fragShaderInfo.codeSize = fragSpv.size() * 4;
		fragShaderInfo.pCode = fragSpv.data();
		VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &fragShaderInfo, NULL, &fragModule));

		// store them for destruction later on
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

	auto guiStages = initStages(gui_frag_spv_data);

	auto image1DStages = initStages(image_frag_1DArray_spv_data);
	auto uimage1DStages = initStages(image_frag_u1DArray_spv_data);
	auto iimage1DStages = initStages(image_frag_i1DArray_spv_data);

	auto image2DStages = initStages(image_frag_2DArray_spv_data);
	auto uimage2DStages = initStages(image_frag_u2DArray_spv_data);
	auto iimage2DStages = initStages(image_frag_i2DArray_spv_data);

	auto image3DStages = initStages(image_frag_3D_spv_data);
	auto uimage3DStages = initStages(image_frag_u3D_spv_data);
	auto iimage3DStages = initStages(image_frag_i3D_spv_data);

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
	colorAttach[0].colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;

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

	std::vector<VkGraphicsPipelineCreateInfo> gpis;

	auto& guiGpi = gpis.emplace_back();
	guiGpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	guiGpi.flags = 0;
	guiGpi.stageCount = 2;
	guiGpi.pStages = guiStages.data();
	guiGpi.pVertexInputState = &vertexInfo;
	guiGpi.pInputAssemblyState = &iaInfo;
	guiGpi.pViewportState = &viewportInfo;
	guiGpi.pRasterizationState = &rasterInfo;
	guiGpi.pMultisampleState = &msInfo;
	guiGpi.pDepthStencilState = &depthInfo;
	guiGpi.pColorBlendState = &blendInfo;
	guiGpi.pDynamicState = &dynState;
	guiGpi.layout = dev.renderData->pipeLayout;
	guiGpi.renderPass = rp_;
	guiGpi.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

	VkGraphicsPipelineCreateInfo imgGpi = guiGpi;
	imgGpi.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
	imgGpi.basePipelineIndex = 0u;
	imgGpi.pStages = image1DStages.data();
	gpis.push_back(imgGpi);

	auto addImGpi = [&](auto& stages) {
		VkGraphicsPipelineCreateInfo gpi = imgGpi;
		gpi.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
		gpi.basePipelineIndex = 1u;
		gpi.pStages = stages.data();
		gpis.push_back(gpi);
	};

	addImGpi(uimage1DStages);
	addImGpi(iimage1DStages);

	addImGpi(image2DStages);
	addImGpi(uimage2DStages);
	addImGpi(iimage2DStages);

	addImGpi(image3DStages);
	addImGpi(uimage3DStages);
	addImGpi(iimage3DStages);

	VkPipeline pipes[10];
	dlg_assert(gpis.size() == sizeof(pipes) / sizeof(pipes[0]));

	VK_CHECK(dev.dispatch.CreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, u32(gpis.size()), gpis.data(), nullptr, pipes));

	pipes_.gui = pipes[0];

	pipes_.image1D = pipes[1];
	pipes_.uimage1D = pipes[2];
	pipes_.iimage1D = pipes[3];

	pipes_.image2D = pipes[4];
	pipes_.uimage2D = pipes[5];
	pipes_.iimage2D = pipes[6];

	pipes_.image3D = pipes[7];
	pipes_.uimage3D = pipes[8];
	pipes_.iimage3D = pipes[9];

	nameHandle(dev, pipes_.gui, "Gui:pipeGui");

	nameHandle(dev, pipes_.image1D, "Gui:pipeImage1D");
	nameHandle(dev, pipes_.uimage1D, "Gui:pipeUImage1D");
	nameHandle(dev, pipes_.iimage1D, "Gui:pipeIImage1D");

	nameHandle(dev, pipes_.image2D, "Gui:pipeImage2D");
	nameHandle(dev, pipes_.uimage2D, "Gui:pipeUImage2D");
	nameHandle(dev, pipes_.iimage2D, "Gui:pipeIImage2D");

	nameHandle(dev, pipes_.image3D, "Gui:pipeImage3D");
	nameHandle(dev, pipes_.uimage3D, "Gui:pipeUImage3D");
	nameHandle(dev, pipes_.iimage3D, "Gui:pipeIImage3D");

	dev.dispatch.DestroyShaderModule(dev.handle, vertModule, nullptr);
	for(auto& mod : modules) {
		dev.dispatch.DestroyShaderModule(dev.handle, mod, nullptr);
	}

	// Init imgui
	this->imgui_ = ImGui::CreateContext();
	ImGui::SetCurrentContext(imgui_);
	this->io_ = &ImGui::GetIO();
	this->io_->IniFilename = nullptr;
	this->io_->MouseDrawCursor = false;

	auto& io = *this->io_;
	io.KeyMap[ImGuiKey_A] = swa_key_a;
	io.KeyMap[ImGuiKey_C] = swa_key_c;
	io.KeyMap[ImGuiKey_V] = swa_key_v;
	io.KeyMap[ImGuiKey_X] = swa_key_x;
	io.KeyMap[ImGuiKey_Y] = swa_key_y;
	io.KeyMap[ImGuiKey_Z] = swa_key_z;
	io.KeyMap[ImGuiKey_Enter] = swa_key_enter;
	io.KeyMap[ImGuiKey_Delete] = swa_key_del;
	io.KeyMap[ImGuiKey_Space] = swa_key_space;
	io.KeyMap[ImGuiKey_LeftArrow] = swa_key_left;
	io.KeyMap[ImGuiKey_DownArrow] = swa_key_down;
	io.KeyMap[ImGuiKey_RightArrow] = swa_key_right;
	io.KeyMap[ImGuiKey_UpArrow] = swa_key_up;
	io.KeyMap[ImGuiKey_Escape] = swa_key_escape;
	io.KeyMap[ImGuiKey_Tab] = swa_key_tab;
	io.KeyMap[ImGuiKey_Backspace] = swa_key_backspace;

	static const ImWchar rangesBasic[] = {
		0x0020, 0x00FF, // Basic Latin + Latin Supplement
		0x03BC, 0x03BC, // micro
		0x03C3, 0x03C3, // small sigma
		0x2013, 0x2013, // en dash
		0x2264, 0x2264, // less-than or equal to
		0,
	};

	io.Fonts->AddFontFromMemoryCompressedTTF(Arimo_compressed_data,
		Arimo_compressed_size, 15.f, nullptr, rangesBasic);

	// Apply style
	ImGui::StyleColorsDark();
	auto& style = ImGui::GetStyle();

	auto accentHue = 0.f; // red

	auto setAccentColorHue = [&](ImGuiCol_ col, u8 h, u8 s, u8 v, u8 a) {
		float r, g, b;
		ImGui::ColorConvertHSVtoRGB(h / 255.f, s / 255.f, v / 255.f, r, g, b);
		style.Colors[col] = {r, g, b, a / 255.f};
	};

	// hsv + alpha. H will always be accentHue
	auto setAccentColor = [&](ImGuiCol_ col, u8 s, u8 v, u8 a) {
		setAccentColorHue(col, accentHue, s, v, a);
	};

	setAccentColor(ImGuiCol_CheckMark, 187, 250, 255);
	setAccentColor(ImGuiCol_SliderGrab, 185, 224, 255);
	setAccentColor(ImGuiCol_SliderGrabActive, 187, 250, 255);

	setAccentColor(ImGuiCol_Button, 187, 250, 102);
	setAccentColor(ImGuiCol_ButtonHovered, 187, 250, 255);
	setAccentColor(ImGuiCol_ButtonActive, 187, 239, 255);

	setAccentColor(ImGuiCol_Header, 187, 250, 79);
	setAccentColor(ImGuiCol_HeaderHovered, 187, 250, 204);
	setAccentColor(ImGuiCol_HeaderActive, 187, 250, 255);

	style.Colors[ImGuiCol_Separator].w = 0.1;
	setAccentColor(ImGuiCol_SeparatorHovered, 221, 191, 150);
	setAccentColor(ImGuiCol_SeparatorActive, 221, 191, 255);

	setAccentColor(ImGuiCol_ResizeGrip, 187, 250, 50);
	setAccentColor(ImGuiCol_ResizeGripHovered, 187, 250, 170);
	setAccentColor(ImGuiCol_ResizeGripActive, 187, 250, 240);

	setAccentColor(ImGuiCol_Tab, 176, 148, 100);
	setAccentColor(ImGuiCol_TabHovered, 187, 250, 200);
	setAccentColor(ImGuiCol_TabActive, 180, 173, 255);

	setAccentColor(ImGuiCol_FrameBg, 170, 122, 138);
	setAccentColor(ImGuiCol_FrameBgHovered, 187, 170, 150);
	setAccentColor(ImGuiCol_FrameBgActive, 187, 190, 240);

	style.Colors[ImGuiCol_WindowBg] = {0.02, 0.02, 0.02, 0.6}; // dark

	setAccentColorHue(ImGuiCol_PlotHistogram, 119, 187, 250, 240);
	setAccentColorHue(ImGuiCol_PlotHistogramHovered, accentHue, 187, 250, 240);

	style.Colors[ImGuiCol_TitleBgActive] = style.Colors[ImGuiCol_WindowBg];
	style.Colors[ImGuiCol_TitleBg] = style.Colors[ImGuiCol_WindowBg];
	style.Colors[ImGuiCol_TitleBgCollapsed] = style.Colors[ImGuiCol_WindowBg];

	// Disable all rounding
	style.WindowRounding = 0.f;
	style.WindowBorderSize = 0.f;
	style.FrameBorderSize = 0.f;
	style.TabRounding = 0.f;
	style.PopupRounding = 0.f;
	style.GrabRounding = 0.f;
	style.ScrollbarRounding = 0.f;

	// Space a bit more vertically, makes information look less overwhelming.
	// Don't overdo it though, we intentionally want it compact.
	style.ItemSpacing = {8, 6};
	style.FramePadding = {6, 6};
	style.ItemInnerSpacing = {4, 4};

	// Center window title
	style.WindowTitleAlign = {0.5f, 0.5f};
	style.Alpha = 1.f;

	// init tabs
	tabs_.resources.gui_ = this;
	tabs_.cb.init(*this);
}

// ~Gui
Gui::~Gui() {
	if(!dev_) {
		return;
	}

	// TODO: needs a lock. Likely also shouldn't be here.
	dlg_assert(dev_->gui == this);
	dev_->gui = nullptr;

	waitForDraws();
	draws_.clear();

	if(imgui_) {
		ImGui::DestroyContext(imgui_);
	}

	auto vkDev = dev_->handle;
	dev_->dispatch.DestroyBuffer(vkDev, font_.uploadBuf, nullptr);
	dev_->dispatch.FreeMemory(vkDev, font_.uploadMem, nullptr);
	dev_->dispatch.DestroyImageView(vkDev, font_.view, nullptr);
	dev_->dispatch.DestroyImage(vkDev, font_.image, nullptr);
	dev_->dispatch.FreeMemory(vkDev, font_.mem, nullptr);

	dev_->dispatch.DestroyPipeline(vkDev, pipes_.gui, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.image1D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.uimage1D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.iimage1D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.image2D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.uimage2D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.iimage2D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.image3D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.uimage3D, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.iimage3D, nullptr);

	dev_->dispatch.DestroyRenderPass(vkDev, rp_, nullptr);
	dev_->dispatch.DestroyCommandPool(vkDev, commandPool_, nullptr);
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
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, font_.uploadMem, 0, VK_WHOLE_SIZE, 0, (void**)(&map)));
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
	ZoneScoped;
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
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, draw.vertexBuffer.mem, 0, VK_WHOLE_SIZE, 0, (void**) &verts));

	ImDrawIdx* inds;
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, draw.indexBuffer.mem, 0, VK_WHOLE_SIZE, 0, (void**) &inds));

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

void Gui::recordDraw(Draw& draw, VkExtent2D extent, VkFramebuffer,
		const ImDrawData& drawData) {
	ZoneScoped;

	auto& dev = *dev_;
	if(drawData.TotalIdxCount == 0 && !clear_) {
		return;
	}

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
				if(cmd.UserCallback) {
					dlg_assert(cmd.UserCallback != ImDrawCallback_ResetRenderState);
					cmd.UserCallback(&cmds, &cmd);

					// reset state we need
					dev.dispatch.CmdPushConstants(draw.cb, dev.renderData->pipeLayout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcr), pcr);
					dev.dispatch.CmdSetViewport(draw.cb, 0, 1, &viewport);
					dev.dispatch.CmdBindVertexBuffers(draw.cb, 0, 1, &draw.vertexBuffer.buf, &off0);
					dev.dispatch.CmdBindIndexBuffer(draw.cb, draw.indexBuffer.buf, 0, VK_INDEX_TYPE_UINT16);
					dev.dispatch.CmdPushConstants(draw.cb, dev.renderData->pipeLayout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcr), pcr);
				} else {
					VkDescriptorSet ds = dsFont_;
					VkPipeline pipe = pipes_.gui;
					auto img = (DrawGuiImage*) cmd.TextureId;
					if(img && img->type != DrawGuiImage::font) {
						ds = draw.dsSelected;
						pipe = [&]{
							switch(img->type) {
								case DrawGuiImage::f1d: return pipes_.image1D;
								case DrawGuiImage::u1d: return pipes_.uimage1D;
								case DrawGuiImage::i1d: return pipes_.iimage1D;

								case DrawGuiImage::f2d: return pipes_.image2D;
								case DrawGuiImage::u2d: return pipes_.uimage2D;
								case DrawGuiImage::i2d: return pipes_.iimage2D;

								case DrawGuiImage::f3d: return pipes_.image3D;
								case DrawGuiImage::u3d: return pipes_.uimage3D;
								case DrawGuiImage::i3d: return pipes_.iimage3D;

								default:
									dlg_error("unreachable");
									return VkPipeline {};
							}
						}();

						// bind push constant data
						struct PcrImageData {
							float layer;
							float valMin;
							float valMax;
							u32 flags;
							float level;
						} pcr = {
							img->layer,
							img->minValue,
							img->maxValue,
							img->flags,
							img->level,
						};

						dev.dispatch.CmdPushConstants(draw.cb, dev.renderData->pipeLayout,
							VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 16,
							sizeof(pcr), &pcr);
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
			}

			vtxOff += cmds.VtxBuffer.Size;
		}
	}
}

std::vector<std::string> enabledFeatures(Device& dev) {
	std::vector<std::string> ret;

#define FEATURE(x) if(dev.enabledFeatures.x) ret.push_back(#x);
	FEATURE(robustBufferAccess);
	FEATURE(fullDrawIndexUint32);
	FEATURE(imageCubeArray);
	FEATURE(independentBlend);
	FEATURE(geometryShader);
	FEATURE(tessellationShader);
	FEATURE(sampleRateShading);
	FEATURE(dualSrcBlend);
	FEATURE(logicOp);
	FEATURE(multiDrawIndirect);
	FEATURE(drawIndirectFirstInstance);
	FEATURE(depthClamp);
	FEATURE(depthBiasClamp);
	FEATURE(fillModeNonSolid);
	FEATURE(depthBounds);
	FEATURE(wideLines);
	FEATURE(largePoints);
	FEATURE(alphaToOne);
	FEATURE(multiViewport);
	FEATURE(samplerAnisotropy);
	FEATURE(textureCompressionETC2);
	FEATURE(textureCompressionASTC_LDR);
	FEATURE(textureCompressionBC);
	FEATURE(occlusionQueryPrecise);
	FEATURE(pipelineStatisticsQuery);
	FEATURE(vertexPipelineStoresAndAtomics);
	FEATURE(fragmentStoresAndAtomics);
	FEATURE(shaderTessellationAndGeometryPointSize);
	FEATURE(shaderImageGatherExtended);
	FEATURE(shaderStorageImageExtendedFormats);
	FEATURE(shaderStorageImageMultisample);
	FEATURE(shaderStorageImageReadWithoutFormat);
	FEATURE(shaderStorageImageWriteWithoutFormat);
	FEATURE(shaderUniformBufferArrayDynamicIndexing);
	FEATURE(shaderSampledImageArrayDynamicIndexing);
	FEATURE(shaderStorageBufferArrayDynamicIndexing);
	FEATURE(shaderStorageImageArrayDynamicIndexing);
	FEATURE(shaderClipDistance);
	FEATURE(shaderCullDistance);
	FEATURE(shaderFloat64);
	FEATURE(shaderInt64);
	FEATURE(shaderInt16);
	FEATURE(shaderResourceResidency);
	FEATURE(shaderResourceMinLod);
	FEATURE(sparseBinding);
	FEATURE(sparseResidencyBuffer);
	FEATURE(sparseResidencyImage2D);
	FEATURE(sparseResidencyImage3D);
	FEATURE(sparseResidency2Samples);
	FEATURE(sparseResidency4Samples);
	FEATURE(sparseResidency8Samples);
	FEATURE(sparseResidency16Samples);
	FEATURE(sparseResidencyAliased);
	FEATURE(variableMultisampleRate);
	FEATURE(inheritedQueries);
#undef FEATURE

	return ret;
}

void Gui::drawOverviewUI(Draw& draw) {
	(void) draw;

	auto& dev = *this->dev_;
	auto& ini = *dev.ini;

	// instance info
	ImGui::Columns(2);

	ImGui::Text("API Version");

	if(ini.app.valid) {
		ImGui::Text("Application");
		ImGui::Text("Engine");
	}

	ImGui::NextColumn();

	ImGui::Text("%d.%d.%d",
		VK_VERSION_MAJOR(ini.app.apiVersion),
		VK_VERSION_MINOR(ini.app.apiVersion),
		VK_VERSION_PATCH(ini.app.apiVersion));

	if(ini.app.valid) {
		ImGui::Text("%s %d.%d.%d", ini.app.name.c_str(),
			VK_VERSION_MAJOR(ini.app.version),
			VK_VERSION_MINOR(ini.app.version),
			VK_VERSION_PATCH(ini.app.version));
		ImGui::Text("%s %d.%d.%d", ini.app.engineName.c_str(),
			VK_VERSION_MAJOR(ini.app.engineVersion),
			VK_VERSION_MINOR(ini.app.engineVersion),
			VK_VERSION_PATCH(ini.app.engineVersion));
	}

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
	ImGui::Separator();

	// Enabled instance extensions
	auto iniExtLbl = dlg::format("{} instance extensions enabled", dev.ini->extensions.size());
	if(ImGui::TreeNode(iniExtLbl.c_str())) {
		ImGui::Indent();
		for(auto& ext : dev.ini->extensions) {
			imGuiText("{}", ext);
		}
		ImGui::Unindent();
		ImGui::TreePop();
	}

	auto devExtLbl = dlg::format("{} device extensions enabled", dev.appExts.size());
	if(ImGui::TreeNode(devExtLbl.c_str())) {
		ImGui::Indent();
		for(auto& ext : dev.appExts) {
			imGuiText("{}", ext);
		}
		ImGui::Unindent();
		ImGui::TreePop();
	}

	auto features = enabledFeatures(dev);
	auto featuresLbl = dlg::format("{} device features enabled", features.size());
	if(ImGui::TreeNode(featuresLbl.c_str())) {
		ImGui::Indent();
		for(auto& f : features) {
			imGuiText("{}", f);
		}
		ImGui::Unindent();
		ImGui::TreePop();
	}

	ImGui::Separator();

	if(dev.swapchain) {
		if(ImGui::Button("View per-frame submissions")) {
			cbGui().showSwapchainSubmissions();
			activateTab(Tab::commandBuffer);
		} else if(showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"This will open the tab to view all submissions done between two\n"
				"presents to the main swapchain. You can alternatively select\n"
				"specific Command Buffers from the 'Resources' tab to view their content.");
		}

		// show timings
		std::vector<float> hist;
		for(auto& timing : dev.swapchain->frameTimings) {
			using MS = std::chrono::duration<float, std::ratio<1, 1000>>;
			hist.push_back(std::chrono::duration_cast<MS>(timing).count());
		}

		// TODO: the histogram has several problems:
		// - very high outliers will render all other timings useless (since
		//   they are scaled down too much)
		// - the variable scaling the make it weird to get an absolute
		//   idea of the timings, only relative is possible
		if(!hist.empty()) {
			ImGui::Text("Present timings:");

			ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.f, 0.f, 0.f, 0.f});
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.f, 0.f, 0.f, 0.f});
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.f, 0.f, 0.f, 0.f});

			float w = ImGui::GetContentRegionAvail().x;
			ImGui::PlotHistogram("", hist.data(), int(hist.size()),
				0, nullptr, 0.f, FLT_MAX, {w, 100});

			ImGui::PopStyleColor();
			ImGui::PopStyleColor();
			ImGui::PopStyleColor();
		}
	}

	// pretty much just own debug stuff
	ImGui::Separator();

	if(checkEnvBinary("VIL_DEBUG", true)) {
		auto& stats = DebugStats::get();
		imGuiText("alive records: {}", stats.aliveRecords);
		imGuiText("alive descriptor sets: {}", stats.aliveDescriptorSets);
		imGuiText("alive descriptor states: {}", stats.aliveDescriptorStates);
		imGuiText("alive buffers: {}", stats.aliveBuffers);
		imGuiText("alive image views: {}", stats.aliveImagesViews);
		imGuiText("threadContext memory: {} KB", stats.threadContextMem / 1024.f);
		imGuiText("command memory: {} KB", stats.commandMem / 1024.f);
		imGuiText("dsState memory: {} KB", stats.descriptorStateMem / 1024.f);
		imGuiText("alive hook records: {}", stats.aliveHookRecords);
		imGuiText("alive hook states: {}", stats.aliveHookStates);
		ImGui::Separator();
		imGuiText("timeline semaphores: {}", dev.timelineSemaphores);
		imGuiText("transform feedback: {}", dev.transformFeedback);
		imGuiText("wrap command buffers: {}", HandleDesc<VkCommandBuffer>::wrap);
		imGuiText("wrap image view: {}", HandleDesc<VkImageView>::wrap);
		imGuiText("wrap buffers: {}", HandleDesc<VkBuffer>::wrap);
		imGuiText("wrap descriptor set: {}", HandleDesc<VkDescriptorSet>::wrap);
		imGuiText("wrap samplers: {}", HandleDesc<VkSampler>::wrap);
		imGuiText("wrap device: {}", HandleDesc<VkDevice>::wrap);
		ImGui::Separator();
		imGuiText("submission counter: {}", dev.submissionCounter);
		imGuiText("pending submissions: {}", dev.pending.size());
		imGuiText("fence pool size: {}", dev.fencePool.size());
		imGuiText("semaphore pool size: {}", dev.semaphorePool.size());
		imGuiText("reset semaphores size: {}", dev.resetSemaphores.size());
	}

	// if(ImGui::TreeNode("Statistics")) {
	// 	auto numGroups = 0u;
	// 	for(auto& qf : dev.queueFamilies) {
	// 		numGroups += unsigned(qf.commandGroups.size());
	// 	}
	// 	imGuiText("Number of command groups: {}", numGroups);
	// 	ImGui::TreePop();
	// }
}

void Gui::drawMemoryUI(Draw&) {
	// TODO:
	// - display graphs instead of just the table
	// - show memory types?
	// - show the biggest actual allocations; some more statistics in general

	// accumulate allocation sizes per heap
	// TODO: cache this.
	auto& memProps = dev().memProps;
	VkDeviceSize heapAlloc[VK_MAX_MEMORY_HEAPS] {};

	for(auto& [_, mem] : dev().deviceMemories.map) {
		auto heap = memProps.memoryTypes[mem->typeIndex].heapIndex;
		heapAlloc[heap] += mem->size;
	}

	VkPhysicalDeviceMemoryBudgetPropertiesEXT memBudget {};
	auto hasMemBudget = contains(dev().allExts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	auto cols = 3u;
	if(hasMemBudget) {
		auto& ini = nonNull(dev().ini);
		dlg_assert(ini.dispatch.GetPhysicalDeviceMemoryProperties2);

		memBudget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

		VkPhysicalDeviceMemoryProperties2 memProps2 {};
		memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
		memProps2.pNext = &memBudget;

		ini.dispatch.GetPhysicalDeviceMemoryProperties2(dev().phdev, &memProps2);

		cols += 2;
	}

	auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders;
	if(ImGui::BeginTable("Memory Heaps", cols, flags)) {
		ImGui::TableSetupColumn("Heap");

		ImGui::TableSetupColumn("Heap Size");
		ImGui::TableSetupColumn("Sum of Allocs");

		if(hasMemBudget) {
			ImGui::TableSetupColumn("Heap Budget");
			ImGui::TableSetupColumn("Heap Own Usage");
		}

		ImGui::TableHeadersRow();

		auto printVal = [&](auto val) {
			// TODO: not always use MB, switch dynamically based on size
			auto block = 1024.f * 1024.f;
			auto prec = block > 10.f ? 0u : 2u;
			imGuiText("{}{}{} MB", std::fixed, std::setprecision(prec), val / block);
		};

		for(auto i = 0u; i < memProps.memoryHeapCount; ++i) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			auto add = "";
			if(memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
				add = " [dev]";
			}

			imGuiText("{}{}", i, add);


			ImGui::TableNextColumn();
			auto sizeMB = memProps.memoryHeaps[i].size;
			printVal(sizeMB);

			ImGui::TableNextColumn();
			auto allocMB = heapAlloc[i];
			printVal(allocMB);

			if(hasMemBudget) {
				ImGui::TableNextColumn();
				auto budgetMB = memBudget.heapBudget[i];
				printVal(budgetMB);

				ImGui::TableNextColumn();
				auto usageMB = memBudget.heapUsage[i];
				printVal(usageMB);
			}
		}

		ImGui::EndTable();
	}
}

void Gui::draw(Draw& draw, bool fullscreen) {
	ZoneScoped;
	resourcesTabDrawn_ = false;

	ImGui::NewFrame();

	unsigned flags = ImGuiWindowFlags_NoCollapse;
	if(fullscreen) {
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		flags = ImGuiWindowFlags_NoDecoration;
	} else {
		// ImGui::ShowDemoWindow();
		// ImGui::ShowAboutWindow();
		// ImGui::ShowMetricsWindow();
		ImGui::SetNextWindowPos({80, 80}, ImGuiCond_Once);
		ImGui::SetNextWindowSize({900, 550}, ImGuiCond_Once);

		// flags |= ImGuiWindowFlags_NoCollapse;
	}

	auto checkSelectTab = [&](Tab tab) {
		auto flags = 0;
		if(activeTab_ == tab && activateTabCounter_ < 2) {
			flags = ImGuiTabItemFlags_SetSelected;
			++activateTabCounter_;
		}

		return flags;
	};

	if(ImGui::Begin("Vulkan Introspection", nullptr, flags)) {
		windowPos_ = ImGui::GetWindowPos();
		windowSize_ = ImGui::GetWindowSize();

		if(ImGui::BeginTabBar("MainTabBar")) {
			if(ImGui::BeginTabItem("Overview")) {
				activeTab_ = Tab::overview;
				drawOverviewUI(draw);
				ImGui::EndTabItem();
			}

			if(ImGui::BeginTabItem("Resources", nullptr, checkSelectTab(Tab::resources))) {
				// When switching towards the resources tab, make sure to refresh
				// the list of available resources, not showing "<Destroyed>"
				if(activeTab_ != Tab::resources) {
					tabs_.resources.firstUpdate_ = true;
					activeTab_ = Tab::resources;
				}

				tabs_.resources.draw(draw);
				ImGui::EndTabItem();
				resourcesTabDrawn_ = true;
			}

			if(ImGui::BeginTabItem("Memory", nullptr, checkSelectTab(Tab::memory))) {
				activeTab_ = Tab::memory;
				drawMemoryUI(draw);
				ImGui::EndTabItem();
			}

			if(tabs_.cb.record_ || tabs_.cb.mode_ == CommandBufferGui::UpdateMode::swapchain) {
				if(ImGui::BeginTabItem("Commands", nullptr, checkSelectTab(Tab::commandBuffer))) {
					activeTab_ = Tab::commandBuffer;
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
	ExtZoneScoped;

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

	// TODO: call locks mutex, we can't do that here tho
	// for(auto* draw : draws) {
	// 	finished(*draw);
	// }
}

void Gui::activateTab(Tab tab) {
	activeTab_ = tab;
	activateTabCounter_ = 0u;

	// When switching towards the resources tab, make sure to refresh
	// the list of available resources, not showing "<Destroyed>"
	if(tab == Tab::resources) {
		tabs_.resources.firstUpdate_ = true;
	}
}

VkResult Gui::renderFrame(FrameInfo& info) {
	ZoneScoped;
	FrameMark;

	makeImGuiCurrent();
	Draw* foundDraw {};

	// find a free draw object
	for(auto& draw : draws_) {
		if(!draw.inUse) {
			foundDraw = &draw;
			continue;
		}

		if(dev().dispatch.GetFenceStatus(dev().handle, draw.fence) == VK_SUCCESS) {
			std::lock_guard devMutex(dev().mutex);
			finishedLocked(draw);
			foundDraw = &draw;
		}
	}

	if(!foundDraw) {
		foundDraw = &draws_.emplace_back();
		foundDraw->init(dev(), commandPool_);
	}

	auto& draw = *foundDraw;
	draw.usedHandles.clear();
	foundDraw->lastUsed = ++drawCounter_;

	VkCommandBufferBeginInfo cbBegin {};
	cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(dev().dispatch.BeginCommandBuffer(draw.cb, &cbBegin));

	ensureFontAtlas(draw.cb);

	auto blurred = false;
	if(blur_.dev) {
		auto& sc = dev().swapchains.get(info.swapchain);
		if(sc.supportsSampling) {
			if(blurSwapchain_ != info.swapchain) {
				// TODO: would have to wait on previous draw here.
				// Not sure if realistic, old swapchain was probably destroyed
				// when we land here?
				vil::resize(blur_, sc.ci.imageExtent, sc.handle, sc.ci.imageFormat);
				blurSwapchain_ = info.swapchain;

				VkDescriptorImageInfo imgInfo {};
				imgInfo.imageView = blur_.view0;
				imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imgInfo.sampler = dev().renderData->linearSampler;

				VkWriteDescriptorSet write {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.pImageInfo = &imgInfo;
				write.descriptorCount = 1u;
				write.dstArrayElement = 0u;
				write.dstSet = blurDs_;
				write.dstBinding = 0u;
				write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

				dev().dispatch.UpdateDescriptorSets(dev().handle, 1u, &write, 0u, nullptr);
			}

			vil::blur(blur_, draw.cb, info.imageIdx, {}, {});
			blurred = true;
		}
	}

	ImGui::GetIO().DisplaySize.x = info.extent.width;
	ImGui::GetIO().DisplaySize.y = info.extent.height;

	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;
	auto now = Clock::now();
	auto diff = now - lastFrame_;
	lastFrame_ = now;
	dt_ = std::chrono::duration_cast<Secf>(diff).count();
	if(dt_ > 0.f) {
		ImGui::GetIO().DeltaTime = dt_;
	}

	VkResult res;

	// TODO: hacky but we have to keep the records alive, making sure
	// it's not destroyed inside the lock. Might need more here for correctness.
	// Should probably come up with better mechanism.
	auto keepAliveBatches = tabs_.cb.records_;
	std::vector<IntrusivePtr<CommandRecord>> keepAliveRecs {
		tabs_.cb.record_,
		tabs_.cb.commandViewer_.record_,
	};

	{
		// Important we already lock this mutex here since we need to make
		// sure no new submissions are done by application while we process
		// and evaluate the pending submissions
		// NOTE: lock order is important here! First lock device mutex,
		// later on lock queue mutex, that's how we must always do it.
		std::lock_guard devMutex(dev().mutex);

		// Clear pending submissions
		for(auto it = dev().pending.begin(); it != dev().pending.end();) {
			auto& subm = *it;
			if(auto nit = checkLocked(*subm); nit) {
				it = *nit;
				continue;
			}

			++it; // already increment to next one so we can't miss it
		}

		// TODO UGH OUCHIE: related to the hack of keeping records alive
		// mentioned above. Yes, this is also terrible, even more so.
		// But we need to make sure they are not destructed (via shared refCount
		// decrease) while we hold the lock.
		for(auto& completed : dev().commandHook->completed) {
			keepAliveRecs.push_back(completed.record);
		}

		this->draw(draw, info.fullscreen);
		auto& drawData = *ImGui::GetDrawData();
		this->uploadDraw(draw, drawData);

		// General barrier to make sure all past submissions writing resources
		// we read are done. Not needed when we don't read a device resource.
		// PERF: could likely at least give a better dstAccessMask
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

		// optionally blur
		VkRenderPassBeginInfo rpBegin {};
		rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rpBegin.renderArea.extent = info.extent;
		rpBegin.renderPass = rp_;
		rpBegin.framebuffer = info.fb;

		VkClearValue clearValues[2] {};

		// color attachment (if we don't clear it, this value is ignored).
		// see render pass creation
		clearValues[0].color = {{0.f, 0.f, 0.f, 1.f}};

		// our depth attachment, always clear that.
		clearValues[1].depthStencil = {1.f, 0u};

		rpBegin.pClearValues = clearValues;
		rpBegin.clearValueCount = 2u;

		dev().dispatch.CmdBeginRenderPass(draw.cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

		if(blurred) {
			VkRect2D scissor;
			// scissor.offset = {};
			// scissor.extent = info.extent;
			scissor.offset.x = std::max(windowPos_.x, 0.f);
			scissor.offset.y = std::max(windowPos_.y, 0.f);
			scissor.extent.width = std::min(
				windowSize_.x + windowPos_.x - scissor.offset.x,
				info.extent.width - windowPos_.x);
			scissor.extent.height = std::min(
				windowSize_.y + windowPos_.y - scissor.offset.y,
				info.extent.height - windowPos_.y);

			VkViewport viewport;
			viewport.minDepth = 0.f;
			viewport.maxDepth = 1.f;
			viewport.x = 0u;
			viewport.y = 0u;
			viewport.width = info.extent.width;
			viewport.height = info.extent.height;
			// viewport.x = windowPos_.x;
			// viewport.y = windowPos_.y;
			// viewport.width = windowSize_.x;
			// viewport.height = windowSize_.y;

			dev().dispatch.CmdSetScissor(draw.cb, 0u, 1u, &scissor);
			dev().dispatch.CmdSetViewport(draw.cb, 0u, 1u, &viewport);

			float pcr[4];
			// scale
			pcr[0] = 1.f;
			pcr[1] = 1.f;
			// translate
			pcr[2] = 0.f;
			pcr[3] = 0.f;

			dev().dispatch.CmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes_.gui);
			dev().dispatch.CmdPushConstants(draw.cb, dev().renderData->pipeLayout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcr), pcr);
			dev().dispatch.CmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
				dev().renderData->pipeLayout, 0u, 1u, &blurDs_, 0, nullptr);
			VkDeviceSize off = 0u;
			dev().dispatch.CmdBindVertexBuffers(draw.cb, 0u, 1u, &blur_.vertices.buf, &off);
			dev().dispatch.CmdDraw(draw.cb, 6, 1, 0, 0);
		}

		this->recordDraw(draw, info.extent, info.fb, drawData);

		dev().dispatch.CmdEndRenderPass(draw.cb);

		if(resourcesTabDrawn_) {
			tabs_.resources.recoredPostRender(draw);
		}

		// General barrier to make sure all our reading is done before
		// future application submissions to this queue.
		// Not needed when we don't read a device resource.
		// PERF: could likely at least give a better srcAccessMask
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

		// == Submit batch ==
		ZoneScopedN("BuildSubmission");

		VkSubmitInfo submitInfos[2] {};
		// NOTE: draw.futureSemaphore might not be waited upon but we need
		// to signal it. We therefore have to reset it, see below (when not
		// using timeline semaphores). The spec isn't clear about whether
		// it's allowed to first wait and then signal the same semaphore
		// in the same VkSubmitInfo (see e.g. https://www.reddit.com/r/vulkan/comments/6pwuzd).
		// In anv, mesa 21.0.3 we encountered a deadlock when trying to do
		// so though, so we now use 2 VkSubmitInfos when we need to reset
		// futureSemaphore.
		auto needPreSubmit = false;
		auto topOfPpipeStage = VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

		auto& submitInfo = submitInfos[1];
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1u;
		submitInfo.pCommandBuffers = &draw.cb;

		// NOTE: could alternatively retrieve all submissions via
		// handle->refRecords->cb->pending (and handle->descriptos->...)
		// but this should be faster, there are usually only a small
		// number of pending submissions while there might be more recordings
		// referencing a handle.

		std::vector<Submission*> waitSubmissions;
		for(auto& pending : reversed(dev().pending)) {
			// When the pending submission was submitted to the gfxQueue
			// (as the gui draw submissions are), we don't need to sync
			// via semaphore, the pipeline barrier is enough
			if(pending->queue == dev().gfxQueue && !forceGuiQueueSemaphores) {
				continue;
			}

			// We only need to chain the last submission on each
			// queue as semaphores guarantee that everything before
			// has finished. Since dev().pending is ordered (by submission
			// order) we just take the first submission per queue
			auto found = false;
			for(auto& sub : waitSubmissions) {
				if(sub->parent->queue == pending->queue) {
					dlg_assert(sub->parent->queueSubmitID > pending->queueSubmitID);
					found = true;
					break;
				}
			}

			if(found) {
				continue;
			}

			auto subs = needsSyncLocked(*pending, draw);
			if(!subs.empty()) {
				// we only need the last submission from the batch
				// as semaphores guarantee everything before in submission
				// order has finished.
				waitSubmissions.push_back(const_cast<Submission*>(subs.back()));
			}
		}

		u64 signalValues[2];
		std::vector<u64> waitValues;
		VkTimelineSemaphoreSubmitInfo tsInfo {};

		auto waitSems = std::vector(info.waitSemaphores.begin(), info.waitSemaphores.end());
		auto signalSems = std::vector{draw.presentSemaphore, draw.futureSemaphore};

		std::vector<VkPipelineStageFlags> waitStages;
		for(auto i = 0u; i < info.waitSemaphores.size(); ++i) {
			// PERF: we might be able to do better
			waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}

		if(dev().timelineSemaphores) {
			dlg_assert(dev().resetSemaphores.empty());

			tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
			tsInfo.pNext = submitInfo.pNext;

			// wait
			waitValues.resize(waitSems.size()); // initial ones, ignored
			for(auto* sub : waitSubmissions) {
				// PERF: guess we could do better
				waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
				waitSems.push_back(sub->ourSemaphore);
				waitValues.push_back(sub->ourSemaphoreValue);
			}

			dlg_assert(waitValues.size() == waitSems.size());
			tsInfo.waitSemaphoreValueCount = u32(waitValues.size());
			tsInfo.pWaitSemaphoreValues = waitValues.data();

			// signal
			// signalValues[0] is uninitialized by design, should be
			// ignored by driver as signalSemaphores[0] is binary
			++draw.futureSemaphoreValue;
			signalValues[1] = draw.futureSemaphoreValue;

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

			// When this draw's futureSemaphore wasn't used, make sure
			// to reset it before we signal it again. This shouldn't acutally
			// cause an additional wait, we already wait for the last draw.
			if(!draw.futureSemaphoreUsed && draw.futureSemaphoreSignaled) {
				// This is a bit messy, see the declaration of needPreSubmit
				// for more details why this is needed.
				needPreSubmit = true;

				auto& preSubmitInfo = submitInfos[0];
				preSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				preSubmitInfo.pWaitSemaphores = &draw.futureSemaphore;
				preSubmitInfo.pWaitDstStageMask = &topOfPpipeStage;
				preSubmitInfo.waitSemaphoreCount = 1u;
			}

			for(auto* sub : waitSubmissions) {
				// take ownership of sub->ourSemaphore if it's valid
				VkSemaphore sem = sub->ourSemaphore;
				sub->ourSemaphore = {};

				// if sub->ourSemaphore has already been used before, we have
				// to synchronize with the queue via a new semaphore.
				if(!sem) {
					sem = getSemaphoreFromPoolLocked(dev());
					auto res = submitSemaphore(*sub->parent->queue, sem);
					if(res != VK_SUCCESS) {
						dlg_error("vkQueueSubmit error: {}", vk::name(res));
						return res;
					}
				}

				// PERF: guess we could do better
				waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
				waitSems.push_back(sem);
				draw.waitedUpon.push_back(sem);
			}
		}

		dlg_assert(waitStages.size() == waitSems.size());

		submitInfo.signalSemaphoreCount = u32(signalSems.size());
		submitInfo.pSignalSemaphores = signalSems.data();
		submitInfo.pWaitDstStageMask = waitStages.data();
		submitInfo.waitSemaphoreCount = u32(waitSems.size());
		submitInfo.pWaitSemaphores = waitSems.data();

		{
			ZoneScopedN("dispatch.QueueSubmit");

			// PERF: when using timeline semaphores we don't need a
			// fence and can just use the timeline semaphore
			std::lock_guard queueLock(dev().queueMutex);
			res = dev().dispatch.QueueSubmit(dev().gfxQueue->handle,
				needPreSubmit ? 2u : 1u,
				needPreSubmit ? submitInfos : &submitInfo, draw.fence);
		}

		if(res != VK_SUCCESS) {
			dlg_error("vkQueueSubmit error: {}", vk::name(res));
			return res;
		}

		draw.inUse = true;
		draw.futureSemaphoreSignaled = true;
		draw.futureSemaphoreUsed = false;
		lastDraw_ = foundDraw;
	}

	// call down
	VkPresentInfoKHR presentInfo {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pImageIndices = &info.imageIdx;
	presentInfo.pWaitSemaphores = &foundDraw->presentSemaphore;
	presentInfo.waitSemaphoreCount = 1u;
	presentInfo.pSwapchains = &info.swapchain;
	presentInfo.swapchainCount = 1u;
	// TODO: forward pNext for all extensions we know. Really important
	// here, might contain important information.
	// Maybe just forward everything? Warn for unknown types?

	{
		ZoneScopedN("dispatch.QueuePresent");

		std::lock_guard queueLock(dev().queueMutex);
		res = dev().dispatch.QueuePresentKHR(info.presentQueue, &presentInfo);
	}

	if(res != VK_SUCCESS) {
		dlg_error("vkQueuePresentKHR error: {}", vk::name(res));

		// TODO: not sure how to handle this the best
		VK_CHECK(dev().dispatch.WaitForFences(dev().handle, 1,
			&foundDraw->fence, true, UINT64_MAX));

		std::lock_guard lock(dev().mutex);
		finishedLocked(*foundDraw);

		return res;
	}

	return VK_SUCCESS;
}

void Gui::waitForDraws() {
	std::vector<VkFence> fences;
	for(auto& draw : draws_) {
		if(draw.inUse) {
			fences.push_back(draw.fence);
		}
	}

	if(!fences.empty()) {
		VK_CHECK(dev().dispatch.WaitForFences(dev().handle,
			u32(fences.size()), fences.data(), true, UINT64_MAX));
		// we can't reset the draws here
	}
}

void Gui::makeImGuiCurrent() {
	ImGui::SetCurrentContext(imgui_);
}

void Gui::selectResource(Handle& handle, bool activateTab) {
	tabs_.resources.select(handle);
	tabs_.resources.filter_ = handle.objectType;

	if(activateTab) {
		this->activateTab(Tab::resources);
	}
}

Draw* Gui::latestPendingDrawSyncLocked(SubmissionBatch& batch) {
	Draw* ret {};
	for(auto& draw : draws_) {
		if(!draw.inUse || dev().dispatch.GetFenceStatus(dev().handle, draw.fence) != VK_SUCCESS) {
			continue;
		}

		if((!ret || draw.lastUsed > ret->lastUsed) && !needsSyncLocked(batch, draw).empty()) {
			ret = &draw;
		}
	}

	return ret;
}

void Gui::finishedLocked(Draw& draw) {
	dlg_assert(draw.inUse);
	dlg_assert(dev().dispatch.GetFenceStatus(dev().handle, draw.fence) == VK_SUCCESS);

	for(auto semaphore : draw.waitedUpon) {
		dev().semaphorePool.push_back(semaphore);
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
		VK_CHECK(dev().dispatch.InvalidateMappedMemoryRanges(dev().handle, 1u, &range));

		// PERF: with some clever syncing we could probably get around
		// the copy here and instead read from the mapped memory directly.
		auto src = static_cast<std::byte*>(draw.readback.map);
		std::copy(src, src + draw.readback.size, rb.lastRead.begin());
	}

	draw.waitedUpon.clear();
	draw.usedHandles.clear();
	draw.usedHookState.reset();

	draw.readback.offset = 0u;
	draw.readback.size = 0u;
	draw.readback.src = {};

	VK_CHECK(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence));

	draw.inUse = false;
}

// util
void refButton(Gui& gui, Handle& handle) {
	// We need the PushID/PopID since there may be multiple
	// ref buttons with the same label (e.g. for unnamed handles)
	ImGui::PushID(&handle);
	if(ImGui::Button(name(handle).c_str())) {
		gui.selectResource(handle);
	}
	ImGui::PopID();
}

void refButtonOpt(Gui& gui, Handle* handle) {
	if(handle) {
		refButton(gui, *handle);
	}
}

void refButtonExpect(Gui& gui, Handle* handle) {
	dlg_assert_or(handle, return);
	refButton(gui, *handle);
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

void displayImage(Gui& gui, DrawGuiImage& imgDraw,
		const VkExtent3D& extent, VkImageType imgType, VkFormat format,
		const VkImageSubresourceRange& subresources,
		VkOffset3D* viewedTexel, ReadBuf texelData) {
	ImVec2 pos = ImGui::GetCursorScreenPos();

	// respect current mip level?
	float aspect = float(extent.width) / extent.height;

	// TODO: this logic might lead to problems for 1xHUGE images
	float regW = ImGui::GetContentRegionAvail().x - 20.f;

	// TODO: also kinda messy. Need this to make avoid flickering for
	// windows that barely need a scrollbar (adding a scrollbar makes
	// the image smaller, causing the content to not need a scrollbar
	// anymore; we will get flickering).
	auto* win = ImGui::GetCurrentWindowRead();
	if(win->ScrollbarY) {
		regW += ImGui::GetStyle().ScrollbarSize;
	}

	float regH = regW / aspect;

	ImGui::Image((void*) &imgDraw, {regW, regH});

	// Taken pretty much just from the imgui demo
	auto& io = gui.imguiIO();
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		float region_sz = 64.0f;
		float center_x = io.MousePos.x - pos.x;
		float center_y = io.MousePos.y - pos.y;
		float region_x = center_x - region_sz * 0.5f;
		float region_y = center_y - region_sz * 0.5f;
		float zoom = 4.0f;
		if (region_x < 0.0f) { region_x = 0.0f; }
		else if (region_x > regW - region_sz) { region_x = regW - region_sz; }
		if (region_y < 0.0f) { region_y = 0.0f; }
		else if (region_y > regH - region_sz) { region_y = regH - region_sz; }

		// ImGui::Text("Min: (%d, %d)",
		// 	int(extent.width * region_x / regW),
		// 	int(extent.height * region_y / regH)
		// );
		// ImGui::Text("Max: (%d, %d)",
		// 	int(extent.width * (region_x + region_sz) / regW),
		// 	int(extent.height * (region_y + region_sz) / regH)
		// );

		auto w = std::max(extent.width >> u32(imgDraw.level), 1u);
		auto h = std::max(extent.height >> u32(imgDraw.level), 1u);

		auto px = int(w * center_x / regW);
		auto py = int(h * center_y / regH);
		if(viewedTexel) {
			viewedTexel->x = px;
			viewedTexel->y = py;
			viewedTexel->z = extent.depth > 1 ? imgDraw.layer : 0;
		}
		if(!texelData.empty()) {
			// TODO: better formatting of color
			// - based on image format, show int or float
			// - when using float, always have fixed length, avoid flickering
			imGuiText("({}, {}): {}", px, py, read(format, texelData));
		}

		ImVec2 uv0 = ImVec2((region_x) / regW, (region_y) / regH);
		ImVec2 uv1 = ImVec2((region_x + region_sz) / regW, (region_y + region_sz) / regH);
		ImGui::Image((void*) &imgDraw, ImVec2(region_sz * zoom, region_sz * zoom), uv0, uv1);
		ImGui::EndTooltip();
	}

	// Row 1: components
	if(subresources.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		auto numComponents = FormatChannelCount(format);
		imgDraw.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

		ImGui::CheckboxFlags("R", &imgDraw.flags, DrawGuiImage::flagMaskR);
		if(numComponents > 1) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("G", &imgDraw.flags, DrawGuiImage::flagMaskG);
		} else {
			imgDraw.flags &= ~(DrawGuiImage::flagMaskG);
		}

		if(numComponents > 2) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("B", &imgDraw.flags, DrawGuiImage::flagMaskB);
		} else {
			imgDraw.flags &= ~(DrawGuiImage::flagMaskB);
		}

		if(numComponents > 3) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("A", &imgDraw.flags, DrawGuiImage::flagMaskA);
		} else {
			imgDraw.flags &= ~(DrawGuiImage::flagMaskA);
		}

		ImGui::SameLine();
		ImGui::CheckboxFlags("Gray", &imgDraw.flags, DrawGuiImage::flagGrayscale);
	} else {
		VkFlags depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		imgDraw.flags = DrawGuiImage::flagMaskR | DrawGuiImage::flagGrayscale;

		// init
		if(imgDraw.aspect != VK_IMAGE_ASPECT_DEPTH_BIT && imgDraw.aspect != VK_IMAGE_ASPECT_STENCIL_BIT) {
			imgDraw.aspect = (subresources.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ?
				VK_IMAGE_ASPECT_DEPTH_BIT :
				VK_IMAGE_ASPECT_STENCIL_BIT;
		}

		if((subresources.aspectMask & depthStencil) == depthStencil) {
			if(ImGui::RadioButton("Depth", imgDraw.aspect == VK_IMAGE_ASPECT_DEPTH_BIT)) {
				imgDraw.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			}

			ImGui::SameLine();
			if(ImGui::RadioButton("Stencil", imgDraw.aspect == VK_IMAGE_ASPECT_STENCIL_BIT)) {
				imgDraw.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
			}
		} else if(subresources.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
			imgDraw.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		} else if(subresources.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
			imgDraw.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
		} else {
			dlg_error("Unsupported image format, aspect mask: '{}'",
				vk::flagNames(VkImageAspectFlagBits(subresources.aspectMask)));
		}
	}

	// Row 2: layer and mip
	if(extent.depth > 1) {
		// TODO: not very convenient to use for a lot of slices.
		//   make sensitivity absolute, i.e. not dependent on number of slices?
		// TODO: this is weird when the image also has mip levels
		auto maxDepth = std::max((extent.depth >> u32(imgDraw.level)), 1u) - 1u;
		ImGui::SliderFloat("slice", &imgDraw.layer, 0, maxDepth);
	} else if(subresources.layerCount > 1) {
		int layer = int(imgDraw.layer);
		ImGui::SliderInt("Layer", &layer, subresources.baseArrayLayer,
			subresources.baseArrayLayer + subresources.layerCount - 1);
		imgDraw.layer = layer;
	}

	if(subresources.levelCount > 1) {
		int mip = int(imgDraw.level);
		ImGui::SliderInt("Mip", &mip, subresources.baseMipLevel,
			subresources.baseMipLevel + subresources.levelCount - 1);
		imgDraw.level = mip;
	}

	// Row 3: min/max values
	ImGui::DragFloat("Min", &imgDraw.minValue, 0.01);
	ImGui::DragFloat("Max", &imgDraw.maxValue, 0.01);
	// NOTE: could add power/gamma slider here.

	// TODO: ugh, this could be done a bit cleaner...
	if(imgType == VK_IMAGE_TYPE_1D) {
		if(imgDraw.aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
			auto numt = FormatDepthNumericalType(format);
			dlg_assert(numt != VK_FORMAT_NUMERICAL_TYPE_NONE);

			if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) imgDraw.type = DrawGuiImage::i1d;
			else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) imgDraw.type = DrawGuiImage::u1d;
			else imgDraw.type = DrawGuiImage::f1d;
		} else if(imgDraw.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
			auto numt = FormatStencilNumericalType(format);
			dlg_assert(numt != VK_FORMAT_NUMERICAL_TYPE_NONE);

			if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) imgDraw.type = DrawGuiImage::i1d;
			else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) imgDraw.type = DrawGuiImage::u1d;
			else imgDraw.type = DrawGuiImage::f1d;
		} else {
			if(FormatIsSampledFloat(format)) imgDraw.type = DrawGuiImage::f1d;
			if(FormatIsInt(format)) imgDraw.type = DrawGuiImage::i1d;
			if(FormatIsUInt(format)) imgDraw.type = DrawGuiImage::u1d;
		}
	} else if(imgType == VK_IMAGE_TYPE_2D) {
		if(imgDraw.aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
			auto numt = FormatDepthNumericalType(format);
			dlg_assert(numt != VK_FORMAT_NUMERICAL_TYPE_NONE);

			if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) imgDraw.type = DrawGuiImage::i2d;
			else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) imgDraw.type = DrawGuiImage::u2d;
			else imgDraw.type = DrawGuiImage::f2d;
		} else if(imgDraw.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
			auto numt = FormatStencilNumericalType(format);
			dlg_assert(numt != VK_FORMAT_NUMERICAL_TYPE_NONE);

			if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) imgDraw.type = DrawGuiImage::i2d;
			else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) imgDraw.type = DrawGuiImage::u2d;
			else imgDraw.type = DrawGuiImage::f2d;
		} else {
			if(FormatIsSampledFloat(format)) imgDraw.type = DrawGuiImage::f2d;
			if(FormatIsInt(format)) imgDraw.type = DrawGuiImage::i2d;
			if(FormatIsUInt(format)) imgDraw.type = DrawGuiImage::u2d;
		}
	} else if(imgType == VK_IMAGE_TYPE_3D) {
		if(imgDraw.aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
			auto numt = FormatDepthNumericalType(format);
			dlg_assert(numt != VK_FORMAT_NUMERICAL_TYPE_NONE);

			if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) imgDraw.type = DrawGuiImage::i3d;
			else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) imgDraw.type = DrawGuiImage::u3d;
			else imgDraw.type = DrawGuiImage::f3d;
		} else if(imgDraw.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
			auto numt = FormatStencilNumericalType(format);
			dlg_assert(numt != VK_FORMAT_NUMERICAL_TYPE_NONE);

			if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) imgDraw.type = DrawGuiImage::i3d;
			else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) imgDraw.type = DrawGuiImage::u3d;
			else imgDraw.type = DrawGuiImage::f1d;
		} else {
			if(FormatIsSampledFloat(format)) imgDraw.type = DrawGuiImage::f3d;
			if(FormatIsInt(format)) imgDraw.type = DrawGuiImage::i3d;
			if(FormatIsUInt(format)) imgDraw.type = DrawGuiImage::u3d;
		}
	}

	dlg_assertm(imgDraw.type != DrawGuiImage::font,
		"imgType {}, format {}", vk::name(imgType), vk::name(format));

	// TODO: display format and aspect?
}

} // namespace vil
