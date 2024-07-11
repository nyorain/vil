#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/render.hpp>
#include <gui/resources.hpp>
#include <gui/cb.hpp>
#include <gui/fonts.hpp>
#include <gui/fontAwesome.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <layer.hpp>
#include <swapchain.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <stats.hpp>
#include <queue.hpp>
#include <handle.hpp>
#include <data.hpp>
#include <wrap.hpp>
#include <serialize/serialize.hpp>
#include <serialize/bufs.hpp>
#include <command/commands.hpp>
#include <util/util.hpp>
#include <nytl/bytes.hpp>
#include <nytl/vecOps.hpp>
#include <util/profiling.hpp>
#include <imgio/file.hpp>

#include <vil_api.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>

#include <set>
#include <map>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cerrno>

#ifdef VIL_DEBUG
	// NOTE: tmp deubgging tool for the LMM algorithm
	// #define VIL_VIZ_LCS
#endif // DEBUG

#ifdef VIL_VIZ_LCS
	#include <gui/vizlcs.hpp>
#endif // VIL_VIZ_LCS

inline namespace imgui {

thread_local ImGuiContext* __LayerImGui;

}

namespace vil {

// from gui/pipes.cpp
// we factor pipeline creation out in a separate file to improve
// compile times when changing shaders or gui.cpp
void initPipes(Device& dev,
	VkRenderPass rp, VkPipelineLayout renderPipeLayout,
	VkPipelineLayout compPipeLayout,
	VkPipelineLayout histogramPipeLayout,
	Gui::Pipelines& dstPipes, bool manualSRGB);

// Gui
Gui::Gui(Device& dev, VkFormat colorFormat) {
	dev_ = &dev;
	lastFrame_ = Clock::now();
	colorFormat_ = colorFormat;
	depthFormat_ = findDepthFormat(dev);
	dlg_assert(depthFormat_ != VK_FORMAT_UNDEFINED);

	// init command pool
	VkCommandPoolCreateInfo cpci {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = dev.gfxQueue->family;
	VK_CHECK(dev.dispatch.CreateCommandPool(dev.handle, &cpci, nullptr, &commandPool_));
	nameHandle(dev, commandPool_, "Gui");

	// init render stuff
	// descriptor set layout
	auto imguiBindings = std::array {
		vku::descriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT),
	};
	imguiDsLayout_.init(dev, imguiBindings, "imgui");

	auto imgOpBindings = std::array {
		vku::descriptorBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_SHADER_STAGE_COMPUTE_BIT),
		vku::descriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_COMPUTE_BIT, &dev.nearestSampler),
	};
	imgOpDsLayout_.init(dev, imgOpBindings, "imgOp");

	auto histogramBindings = std::array {
		vku::descriptorBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT),
	};
	histogramDsLayout_.init(dev, histogramBindings, "histogram");

	// pipeline layout
	// We just allocate the full push constant range that all implementations
	// must support.
	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.offset = 0;
	// PERF: perf most pipelines don't need this much. Could create multiple
	// pipe layouts.
	pcr.size = 128; // needed e.g. for vertex viewer pipeline

	imguiPipeLayout_ = vku::PipelineLayout(dev,
		{{imguiDsLayout_.vkHandle()}}, {{pcr}}, "imgui");

	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	histogramPipeLayout_ = vku::PipelineLayout(dev,
		{{histogramDsLayout_.vkHandle()}}, {{pcr}}, "histogram");
	imgOpPipeLayout_ = vku::PipelineLayout(dev,
		{{imgOpDsLayout_.vkHandle()}}, {{pcr}}, "imgOp");

	initRenderStuff();
	initImGui();

	// init blur
	auto blurBackground = checkEnvBinary("VIL_BLUR", true);
	if(blurBackground) {
		vil::init(blur_, dev);
		blurDs_ = allocDs(imguiDsLayout_, "Gui:blur");
	}

	// init tabs
	// TODO: use RAII for init
	tabs_.resources = std::make_unique<ResourceGui>();
	tabs_.cb = std::make_unique<CommandRecordGui>();

	tabs_.resources->init(*this);
	tabs_.cb->init(*this);
}

void Gui::destroyRenderStuff() {
	auto vkDev = dev_->handle;

	dev_->dispatch.DestroyPipeline(vkDev, pipes_.gui, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.imageBg, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.histogramPrepare, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.histogramMax, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.histogramPost, nullptr);
	dev_->dispatch.DestroyPipeline(vkDev, pipes_.histogramRender, nullptr);
	for(auto i = 0u; i < ShaderImageType::count; ++i) {
		dev_->dispatch.DestroyPipeline(vkDev, pipes_.image[i], nullptr);
		dev_->dispatch.DestroyPipeline(vkDev, pipes_.readTex[i], nullptr);
		dev_->dispatch.DestroyPipeline(vkDev, pipes_.minMaxTex[i], nullptr);
		dev_->dispatch.DestroyPipeline(vkDev, pipes_.histogramTex[i], nullptr);
	}

	dev_->dispatch.DestroyRenderPass(vkDev, rp_, nullptr);
}

void Gui::initRenderStuff() {
	auto& dev = *dev_;
	destroyRenderStuff();

	// render pass
	VkAttachmentDescription atts[2] {};

	auto& colorAtt = atts[0];
	colorAtt.format = colorFormat_;
	colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAtt.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	auto& depthAtt = atts[1];
	depthAtt.format = depthFormat_;
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

	// imgui outputs srgb colors, need to convert to linear when rendering
	// into srgb swapchain. This seems counterintuitive but is right.
	auto manualSRGB = FormatIsSRGB(colorFormat_);
	initPipes(dev, rp_,
		imguiPipeLayout_.vkHandle(),
		imgOpPipeLayout_.vkHandle(),
		histogramPipeLayout_.vkHandle(),
		pipes_, manualSRGB);
}

void Gui::initImGui() {
	// Init imgui
	this->imgui_ = ImGui::CreateContext();
	ImGui::SetCurrentContext(imgui_);
	this->io_ = &ImGui::GetIO();
	this->io_->IniFilename = nullptr;
	this->io_->MouseDrawCursor = false;

	// TODO: also add gamepad support
	// TODO: figure out how to make our custom selectables (using IsItemClicked) work
	this->io_->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	auto& io = *this->io_;

	static const ImWchar rangesBasic[] = {
		0x0020, 0x00FF, // Basic Latin + Latin Supplement
		0x03BC, 0x03BC, // micro
		0x03C3, 0x03C3, // small sigma
		0x2013, 0x2013, // en dash
		0x2264, 0x2264, // less-than or equal to
		0,
	};

    static const ImWchar rangesIcons[] = {
        ICON_MIN_FA, ICON_MAX_FA,
        0
    };

	ImFontConfig configOwned;
	configOwned.FontDataOwnedByAtlas = false;
    // configOwned.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;

    ImFontConfig configMerge;
    configMerge.MergeMode = true;
    // configMerge.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;

	float scale = 1.0f;
	auto e = std::getenv("VIL_UI_SCALE");
	if(e) {
		auto res = std::strtof(e, nullptr);
		if(res != 0.f) {
			scale = res;
			uiScale_ = scale;
		} else {
			dlg_warn("Environment variable 'VIL_UI_SCALE' set to invalid value '{}'.", e);
		}
	}

	defaultFont = io.Fonts->AddFontFromMemoryCompressedTTF(arimo_compressed_data,
		arimo_compressed_size, scale * 15.f, nullptr, rangesBasic);
	io.Fonts->AddFontFromMemoryCompressedTTF(fontAwesomeSolid_compressed_data,
		fontAwesomeSolid_compressed_size, scale * 14.f, &configMerge, rangesIcons);

	// TODO: compress
	monoFont = io.Fonts->AddFontFromMemoryTTF((void*) inconsolata_compressed_data,
		inconsolata_compressed_size, scale * 15.f, &configOwned, rangesBasic);

	// Apply style
	ImGui::StyleColorsDark();
	auto& style = ImGui::GetStyle();

	auto accentHue = 0.f; // red

	auto getColorHSV = [&](u8 h, u8 s, u8 v, u8 a) {
		float r, g, b;
		ImGui::ColorConvertHSVtoRGB(h / 255.f, s / 255.f, v / 255.f, r, g, b);
		return ImVec4{r, g, b, a / 255.f};
	};

	auto setColorHSV = [&](ImGuiCol_ col, u8 h, u8 s, u8 v, u8 a) {
		style.Colors[col] = getColorHSV(h, s, v, a);
	};

	// hsv + alpha. H will always be accentHue
	auto setAccentColor = [&](ImGuiCol_ col, u8 s, u8 v, u8 a) {
		setColorHSV(col, accentHue, s, v, a);
	};

	setAccentColor(ImGuiCol_CheckMark, 187, 250, 255);
	setAccentColor(ImGuiCol_SliderGrab, 185, 224, 255);
	setAccentColor(ImGuiCol_SliderGrabActive, 187, 250, 255);

	setAccentColor(ImGuiCol_Button, 187, 250, 102);
	setAccentColor(ImGuiCol_ButtonHovered, 187, 250, 255);
	setAccentColor(ImGuiCol_ButtonActive, 187, 239, 255);

	this->inlineButtonCol = getColorHSV(accentHue, 100, 250, 255);
	this->inlineButtonColHovered = getColorHSV(accentHue, 187, 250, 255);
	this->inlineButtonColActive = getColorHSV(accentHue, 187, 239, 255);

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

	// style.Colors[ImGuiCol_WindowBg] = {0.02, 0.02, 0.02, 0.6}; // dark
	// style.Colors[ImGuiCol_WindowBg] = {0.1, 0.1, 0.1, 0.2};
	style.Colors[ImGuiCol_WindowBg] = {0.0, 0.0, 0.0, 0.0};

	setColorHSV(ImGuiCol_PlotHistogram, 119, 240, 180, 240);
	setColorHSV(ImGuiCol_PlotHistogramHovered, accentHue, 187, 250, 240);

	// style.Colors[ImGuiCol_TitleBgActive] = style.Colors[ImGuiCol_WindowBg];
	// style.Colors[ImGuiCol_TitleBg] = style.Colors[ImGuiCol_WindowBg];
	setColorHSV(ImGuiCol_TitleBgActive, 119, 240, 150, 200);
	setColorHSV(ImGuiCol_TitleBg, 119, 100, 100, 50);
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
	style.FramePadding = {4, 4};
	style.ItemInnerSpacing = {4, 4};
	style.CellPadding = {6, 1}; // we need this since we sometimes don't use lines

	// Center window title
	style.WindowTitleAlign = {0.5f, 0.5f};
	style.Alpha = 1.f;

	style.ScaleAllSizes(scale);
}

// ~Gui
Gui::~Gui() {
	if(!dev_) {
		return;
	}

	waitForDraws();
	for(auto& draw : draws_) {
		if(draw->inUse) {
			finishedLocked(*draw);
		}
	}

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

	destroyRenderStuff();
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
	// TODO: use a8
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

	// create and update font descriptor
	dsFont_ = allocDs(imguiDsLayout_, "imguiFont");
	vku::DescriptorUpdate(dsFont_).set(font_.view, dev.linearSampler);

	// Store our identifier
	font_.drawImage.type = ShaderImageType::count; // font
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
	auto vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	auto vertexSize = drawData.TotalVtxCount * sizeof(ImDrawVert);
	draw.vertexBuffer.ensure(dev, vertexSize, vertexUsage);

	auto indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	auto indexSize = drawData.TotalIdxCount * sizeof(ImDrawIdx);
	draw.indexBuffer.ensure(dev, indexSize, indexUsage);

	// map
	ImDrawVert* verts = reinterpret_cast<ImDrawVert*>(draw.vertexBuffer.map);
	ImDrawIdx* inds = reinterpret_cast<ImDrawIdx*>(draw.indexBuffer.map);

	for(auto i = 0; i < drawData.CmdListsCount; ++i) {
		auto& cmds = *drawData.CmdLists[i];
		std::memcpy(verts, cmds.VtxBuffer.Data, cmds.VtxBuffer.size() * sizeof(ImDrawVert));
		std::memcpy(inds, cmds.IdxBuffer.Data, cmds.IdxBuffer.size() * sizeof(ImDrawIdx));
		verts += cmds.VtxBuffer.Size;
		inds += cmds.IdxBuffer.Size;
	}

	draw.indexBuffer.flushMap();
	draw.vertexBuffer.flushMap();
}

void Gui::recordDraw(Draw& draw, VkExtent2D extent, VkFramebuffer,
		const ImDrawData& drawData) {
	ZoneScoped;
	DebugLabel cblbl(dev(), draw.cb, "vil:Gui:recordDraw");

	auto& dev = *dev_;
	if(drawData.TotalIdxCount == 0) {
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
		auto pcrStages = VK_SHADER_STAGE_VERTEX_BIT |
			VK_SHADER_STAGE_FRAGMENT_BIT |
			VK_SHADER_STAGE_COMPUTE_BIT;
		dev.dispatch.CmdPushConstants(draw.cb, imguiPipeLayout_.vkHandle(),
			pcrStages, 0, sizeof(pcr), pcr);

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
					dev.dispatch.CmdPushConstants(draw.cb, imguiPipeLayout_.vkHandle(),
						pcrStages, 0, sizeof(pcr), pcr);
					dev.dispatch.CmdSetViewport(draw.cb, 0, 1, &viewport);
					dev.dispatch.CmdBindVertexBuffers(draw.cb, 0, 1, &draw.vertexBuffer.buf, &off0);
					dev.dispatch.CmdBindIndexBuffer(draw.cb, draw.indexBuffer.buf, 0, VK_INDEX_TYPE_UINT16);
					dev.dispatch.CmdPushConstants(draw.cb, imguiPipeLayout_.vkHandle(),
						pcrStages, 0, sizeof(pcr), pcr);
				} else {
					// TODO: we only ever really need in this im ImageViewer.
					//   Maybe just use it there explicitly instead
					//   of handling it here?
					VkDescriptorSet ds = dsFont_.vkHandle();
					VkPipeline pipe = pipes_.gui;
					auto img = (DrawGuiImage*) cmd.TextureId;
					if(img && img->type != ShaderImageType::count) {
						ds = img->ds;
						dlg_assert(img->type < ShaderImageType::count);
						pipe = pipes_.image[img->type];

						// bind push constant data
						struct PcrImageData {
							float layer;
							float valMin;
							float valMax;
							u32 flags;
							float level;
							float power;
						} pcr = {
							img->layer,
							img->minValue,
							img->maxValue,
							img->flags,
							img->level,
							img->power
						};

						dev.dispatch.CmdPushConstants(draw.cb, imguiPipeLayout_.vkHandle(),
							pcrStages, 16,
							sizeof(pcr), &pcr);
					}

					dev.dispatch.CmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
					dev.dispatch.CmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
						imguiPipeLayout_.vkHandle(), 0, 1, &ds, 0, nullptr);

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

std::vector<std::string> enabledFeatures11(Device& dev) {
	std::vector<std::string> ret;

#define FEATURE(x) if(dev.enabledFeatures11.x) ret.push_back(#x);
    FEATURE(storageBuffer16BitAccess);
    FEATURE(uniformAndStorageBuffer16BitAccess);
    FEATURE(storagePushConstant16);
    FEATURE(storageInputOutput16);
    FEATURE(multiview);
    FEATURE(multiviewGeometryShader);
    FEATURE(multiviewTessellationShader);
    FEATURE(variablePointersStorageBuffer);
    FEATURE(variablePointers);
    FEATURE(protectedMemory);
    FEATURE(samplerYcbcrConversion);
    FEATURE(shaderDrawParameters);
#undef FEATURE

	return ret;
}

std::vector<std::string> enabledFeatures12(Device& dev) {
	std::vector<std::string> ret;

#define FEATURE(x) if(dev.enabledFeatures12.x) ret.push_back(#x);
    FEATURE(samplerMirrorClampToEdge);
    FEATURE(drawIndirectCount);
    FEATURE(storageBuffer8BitAccess);
    FEATURE(uniformAndStorageBuffer8BitAccess);
    FEATURE(storagePushConstant8);
    FEATURE(shaderBufferInt64Atomics);
    FEATURE(shaderSharedInt64Atomics);
    FEATURE(shaderFloat16);
    FEATURE(shaderInt8);
    FEATURE(descriptorIndexing);
    FEATURE(shaderInputAttachmentArrayDynamicIndexing);
    FEATURE(shaderUniformTexelBufferArrayDynamicIndexing);
    FEATURE(shaderStorageTexelBufferArrayDynamicIndexing);
    FEATURE(shaderUniformBufferArrayNonUniformIndexing);
    FEATURE(shaderSampledImageArrayNonUniformIndexing);
    FEATURE(shaderStorageBufferArrayNonUniformIndexing);
    FEATURE(shaderStorageImageArrayNonUniformIndexing);
    FEATURE(shaderInputAttachmentArrayNonUniformIndexing);
    FEATURE(shaderUniformTexelBufferArrayNonUniformIndexing);
    FEATURE(shaderStorageTexelBufferArrayNonUniformIndexing);
    FEATURE(descriptorBindingUniformBufferUpdateAfterBind);
    FEATURE(descriptorBindingSampledImageUpdateAfterBind);
    FEATURE(descriptorBindingStorageImageUpdateAfterBind);
    FEATURE(descriptorBindingStorageBufferUpdateAfterBind);
    FEATURE(descriptorBindingUniformTexelBufferUpdateAfterBind);
    FEATURE(descriptorBindingStorageTexelBufferUpdateAfterBind);
    FEATURE(descriptorBindingUpdateUnusedWhilePending);
    FEATURE(descriptorBindingPartiallyBound);
    FEATURE(descriptorBindingVariableDescriptorCount);
    FEATURE(runtimeDescriptorArray);
    FEATURE(samplerFilterMinmax);
    FEATURE(scalarBlockLayout);
    FEATURE(imagelessFramebuffer);
    FEATURE(uniformBufferStandardLayout);
    FEATURE(shaderSubgroupExtendedTypes);
    FEATURE(separateDepthStencilLayouts);
    FEATURE(hostQueryReset);
    FEATURE(timelineSemaphore);
    FEATURE(bufferDeviceAddress);
    FEATURE(bufferDeviceAddressCaptureReplay);
    FEATURE(bufferDeviceAddressMultiDevice);
    FEATURE(vulkanMemoryModel);
    FEATURE(vulkanMemoryModelDeviceScope);
    FEATURE(vulkanMemoryModelAvailabilityVisibilityChains);
    FEATURE(shaderOutputViewportIndex);
    FEATURE(shaderOutputLayer);
    FEATURE(subgroupBroadcastDynamicId);
#undef FEATURE

	return ret;
}

std::vector<std::string> enabledFeatures13(Device& dev) {
	std::vector<std::string> ret;

#define FEATURE(x) if(dev.enabledFeatures13.x) ret.push_back(#x);
    FEATURE(robustImageAccess);
    FEATURE(inlineUniformBlock);
    FEATURE(descriptorBindingInlineUniformBlockUpdateAfterBind);
    FEATURE(pipelineCreationCacheControl);
    FEATURE(privateData);
    FEATURE(shaderDemoteToHelperInvocation);
    FEATURE(shaderTerminateInvocation);
    FEATURE(subgroupSizeControl);
    FEATURE(computeFullSubgroups);
    FEATURE(synchronization2);
    FEATURE(textureCompressionASTC_HDR);
    FEATURE(shaderZeroInitializeWorkgroupMemory);
    FEATURE(dynamicRendering);
    FEATURE(shaderIntegerDotProduct);
    FEATURE(maintenance4);
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
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, uiScale_ * ImVec2(2.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, uiScale_ * ImVec2(4.f, 4.f));

	auto iniExtLbl = dlg::format("{} instance extensions enabled", dev.ini->extensions.size());
	auto tnFlags = ImGuiTreeNodeFlags_FramePadding;
	if(ImGui::TreeNodeEx(iniExtLbl.c_str(), tnFlags)) {
		ImGui::Indent();
		for(auto& ext : dev.ini->extensions) {
			imGuiText("{}", ext);
		}
		ImGui::Unindent();
		ImGui::TreePop();
	}

	auto devExtLbl = dlg::format("{} device extensions enabled", dev.appExts.size());
	if(ImGui::TreeNodeEx(devExtLbl.c_str(), tnFlags)) {
		ImGui::Indent();
		for(auto& ext : dev.appExts) {
			imGuiText("{}", ext);
		}
		ImGui::Unindent();
		ImGui::TreePop();
	}

	// NOTE: only showing core features atm
	// TODO: extend to cover extensions
	auto features10 = enabledFeatures(dev);
	auto features11 = enabledFeatures11(dev);
	auto features12 = enabledFeatures12(dev);
	auto features13 = enabledFeatures13(dev);

	auto total = features10.size() + features11.size() + features12.size() + features13.size();
	auto featuresLbl = dlg::format("{} Vulkan core device features enabled", total);
	if(ImGui::TreeNodeEx(featuresLbl.c_str(), tnFlags)) {
		auto print = [&](auto& name, auto& features) {
			if(features.empty()) {
				return;
			}

			imGuiText(name);
			ImGui::Indent();
			for(auto& f : features) {
				imGuiText("{}", f);
			}
			ImGui::Unindent();
		};

		print("Vulkan 1.0", features10);
		print("Vulkan 1.1", features11);
		print("Vulkan 1.2", features12);
		print("Vulkan 1.3", features13);

		ImGui::TreePop();
	}

	/*
	auto printFeatures = [&](auto version, auto name, auto featuresFunc) {
		if(dev.props.apiVersion < version) {
			return;
		}

		auto features = featuresFunc(dev);
		auto featuresLbl = dlg::format("{} {} device features enabled", features.size(), name);
		if(ImGui::TreeNodeEx(featuresLbl.c_str(), tnFlags)) {
			ImGui::Indent();
			for(auto& f : features) {
				imGuiText("{}", f);
			}
			ImGui::Unindent();
			ImGui::TreePop();
		}
	};

	printFeatures(VK_API_VERSION_1_0, "Vulkan 1.0", &enabledFeatures);
	printFeatures(VK_API_VERSION_1_1, "Vulkan 1.1", &enabledFeatures11);
	printFeatures(VK_API_VERSION_1_2, "Vulkan 1.2", &enabledFeatures12);
	printFeatures(VK_API_VERSION_1_3, "Vulkan 1.3", &enabledFeatures13);
	*/

	ImGui::PopStyleVar(2);
	ImGui::Separator();

	// swapchain stuff
	auto swapchain = dev.swapchain();

	if(swapchain) {
		if(ImGui::Button("View per-frame submissions")) {
			cbGui().showSwapchainSubmissions(*swapchain);
			activateTab(Tab::commandBuffer);
		} else if(showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"This will open the tab to view all submissions done between two\n"
				"presents to the main swapchain. You can alternatively select\n"
				"specific CommandBuffers from the 'Resources' tab to view their content.");
		}

		auto displayLocalCapture = [&](auto& lc) {
			bool found = false;

			{
				std::lock_guard lock(dev.mutex);
				found = !!lc.completed.state;
			}

			std::string lbl = dlg::format("Local Captures '{}': {}", lc.name,
				 found ? "found" : "not found");
			if(ImGui::Button(lbl.c_str())) {
				cbGui().showLocalCaptures(lc);
				activateTab(Tab::commandBuffer);
			}
		};

		for(auto& lc : dev.commandHook->localCaptures()) {
			displayLocalCapture(*lc);
		}

		for(auto& lc : dev.commandHook->localCapturesOnceCompleted()) {
			displayLocalCapture(*lc);
		}

		// show timings
		std::vector<float> hist;

		{
			std::lock_guard lock(dev.mutex);
			for(auto& timing : swapchain->frameTimings) {
				using MS = std::chrono::duration<float, std::ratio<1, 1000>>;
				hist.push_back(std::chrono::duration_cast<MS>(timing).count());
			}
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
		imGuiText("alive descriptor copies: {}", stats.aliveDescriptorCopies);
		imGuiText("alive buffers: {}", stats.aliveBuffers);
		imGuiText("alive image views: {}", stats.aliveImagesViews);
		imGuiText("threadContext memory: {} MB", stats.threadContextMem / (1024.f * 1024.f));
		imGuiText("command memory: {} MB", stats.commandMem / (1024.f * 1024.f));
		imGuiText("ds copy memory: {} MB", stats.descriptorCopyMem / (1024.f * 1024.f));
		imGuiText("ds pool memory: {} MB", stats.descriptorPoolMem / (1024.f * 1024.f));
		imGuiText("alive hook records: {}", stats.aliveHookRecords);
		imGuiText("alive hook states: {}", stats.aliveHookStates);
		imGuiText("layer buffer memory: {} MB", stats.ownBufferMem / (1024.f * 1024.f));
		imGuiText("layer image memory: {} MB", stats.copiedImageMem / (1024.f * 1024.f));
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

		ImGui::Separator();

		if(dev.timelineSemaphores) {
			auto val = dev.doFullSync.load();
			ImGui::Checkbox("Full-Sync", &val);
			dev.doFullSync.store(val);
			if(ImGui::IsItemHovered() && showHelp) {
				ImGui::SetTooltip("Causes over-conservative synchronization of\n"
					"inserted layer commands.\n"
					"Might fix synchronization in some corner cases\n"
					"and is needed when your application accesses buffers\n"
					"by just using device addresses");
			}
		}

		auto val = dev.captureCmdStack.load();
		ImGui::Checkbox("Capture Command Callstacks", &val);
		dev.captureCmdStack.store(val);
		if(ImGui::IsItemHovered() && showHelp) {
			ImGui::SetTooltip("Captures and shows callstacks of each command");
		}

		ImGui::Checkbox("Show ImGui Demo", &showImguiDemo_);

		auto force = dev.commandHook->forceHook.load();
		if(ImGui::Checkbox("Force hooking", &force)) {
			dev.commandHook->forceHook.store(force);
		}

		auto allow = dev.commandHook->allowReuse.load();
		if(ImGui::Checkbox("Allow hook record reuse", &allow)) {
			dev.commandHook->allowReuse.store(allow);
		}

		auto hookAccel = dev.commandHook->hookAccelStructBuilds.load();
		if(ImGui::Checkbox("Hook AccelerationStructures", &hookAccel)) {
			dev.commandHook->hookAccelStructBuilds.store(hookAccel);
		}
	}
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

	{
		std::lock_guard lock(dev().mutex);
		for(auto& [_, mem] : dev().deviceMemories.inner) {
			auto heap = memProps.memoryTypes[mem->typeIndex].heapIndex;
			heapAlloc[heap] += mem->size;
		}
	}

	VkPhysicalDeviceMemoryBudgetPropertiesEXT memBudget {};
	auto hasMemBudget = contains(dev().allExts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	auto cols = 3u;
	if(hasMemBudget) {
		auto& ini = *dev().ini;
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

	ImGui::NewFrame();

	unsigned flags = ImGuiWindowFlags_NoCollapse;
	if(fullscreen) {
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		flags = ImGuiWindowFlags_NoDecoration;
	} else {
		ImGui::SetNextWindowPos({80, 80}, ImGuiCond_Once);
		ImGui::SetNextWindowSize({900, 550}, ImGuiCond_Once);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

		auto col = focused_ ?
			ImVec4(1.f, 0.5f, 0.5f, 0.8f) :
			ImVec4(0.2f, 0.2f, 0.2f, 0.1f);
		ImGui::PushStyleColor(ImGuiCol_Border, col);
	}

	if(showImguiDemo_) {
		ImGui::ShowDemoWindow();
		ImGui::ShowMetricsWindow();
		// ImGui::ShowAboutWindow();
	}

	// TODO: needed?
	// if(this->focused) {
	// 	ImGui::SetNextWindowFocus();
	// } else {
	// 	ImGui::SetWindowFocus(nullptr);
	// }
	if(this->unfocus) {
		ImGui::SetWindowFocus(nullptr);
		this->unfocus = false;
	}

	if(activeTab_ != Tab::commandBuffer) {
		// deactivate hook when we aren't in the commandbuffer tab
		dev().commandHook->freeze.store(true);
	}

	flags |= ImGuiWindowFlags_NoTitleBar;

	if(mode_ == Mode::normal) {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0.f, 0.f));
	}

	if(ImGui::Begin("Vulkan Introspection", nullptr, flags)) {
		focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		windowPos_ = ImGui::GetWindowPos();
		windowSize_ = ImGui::GetWindowSize();

		if(mode_ == Mode::normal) {
			auto checkSelectTab = [&](Tab tab) {
				auto flags = 0;
				if(activeTab_ == tab && activateTabCounter_ < 2) {
					flags = ImGuiTabItemFlags_SetSelected;
					++activateTabCounter_;
				}

				return flags;
			};

			// tab bar
			auto& dl = *ImGui::GetWindowDrawList();
			auto pos = ImGui::GetCursorScreenPos();
			auto pad = ImVec2(0, 0); // ImGui::GetStyle().WindowPadding;

			// pos.x = ImGui::GetWindowPos().x;
			// pos.y = ImGui::GetCursorScreenPos().y - pad.y;
			// pos.y = ImGui::GetWindowPos().y;
			// pos.x -= pad.x;
			// pos.y -= pad.y;

			auto tabWidth = 100 * uiScale_;
			auto barHeight = 32.f * uiScale_;
			auto barEnd = ImVec2(
				pos.x + ImGui::GetContentRegionAvail().x + 2 * pad.x,
				pos.y + barHeight);

			dl.PushClipRect(pos, barEnd);
			// dl.AddRectFilled(pos, barEnd,
			// 	ImGui::ColorConvertFloat4ToU32(ImVec4(0.1, 0.1, 0.1, 0.1)));

			auto selColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.1, 0.1, 0.1, 0.5));
			auto inactiveColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.3, 0.3, 0.3, 0.4));
			auto hoverColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.4, 0.4, 0.4, 0.7));
			auto pressColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.5, 0.5, 0.5, 0.7));
			auto lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.8, 0.8, 0.8, 0.5));

			ImGui::SetCursorScreenPos(pos);

			auto first = true;
			auto prevSelected = false;

			auto tabItem = [&](const char* label, Tab type) {
				if(checkSelectTab(type)) {
					activeTab_ = type;
				}

				auto selected = (activeTab_ == type);
				auto line = false;
				if(first) {
					first = false;
				} else {
					ImGui::SameLine();
					line = !prevSelected && !selected;
				}

				const auto& style = ImGui::GetStyle();
				auto id = ImGui::GetID(label);

    			const auto labelSize = ImGui::CalcTextSize(label, NULL, true);
				const auto start = ImGui::GetCursorScreenPos();

				const auto baseSize = ImVec2(tabWidth, barHeight);
				const auto size = ImVec2(baseSize.x, baseSize.y);

				if(line) {
					dl.AddLine(start + ImVec2(0.0, 5),
						start + ImVec2(0.0, size.y - 5), lineColor,
						1.f);
				}

    			const ImRect bb(start, start + size);
				ImGui::ItemSize(size, style.FramePadding.y);
				if(!ImGui::ItemAdd(bb, id))
					return;

				bool hovered, held;
				auto pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0u);

				ImGui::RenderNavHighlight(bb, id);

				auto rounding = 0.f;
				if(selected) {
    				const ImU32 col = selColor;
					ImGui::RenderFrame(bb.Min, bb.Max, col, true, rounding);
					// dl.AddRectFilled(bb.Min, bb.Max, col, rounding,
					// 	roundingFlags);
				} else {
    				const ImU32 col =
						held ? pressColor :
						hovered ? hoverColor :
						inactiveColor;
					ImGui::RenderFrame(bb.Min, bb.Max, col, true, rounding);
					// dl.AddRectFilled(bb.Min, bb.Max, col, rounding,
					// 	roundingFlags);
				}

				ImGui::RenderTextClipped(bb.Min + style.FramePadding,
					bb.Max - style.FramePadding, label,
					NULL, &labelSize, ImVec2(0.5f, 0.5f), &bb);

				// auto pressed = ImGui::Button(label);

				if(pressed && activeTab_ != type) {
					activateTab(type);
				}

				prevSelected = selected;
			};

			tabItem(ICON_FA_HOME " Overview", Tab::overview);
			tabItem(ICON_FA_IMAGES " Resources", Tab::resources);
			tabItem(ICON_FA_MEMORY " Memory", Tab::memory);
			tabItem(ICON_FA_LIST " Commands", Tab::commandBuffer);

			ImGui::SameLine();
			const auto start = ImGui::GetCursorScreenPos();
			dl.AddRectFilled(start, barEnd, inactiveColor);
			ImGui::NewLine();

			// end
			ImGui::PopStyleVar(3);
			dl.PopClipRect();

			{
				auto pos = ImGui::GetCursorScreenPos() - pad;
				auto size = ImGui::GetContentRegionAvail() + pad;
				auto end = pos + size;

				dl.PushClipRect(pos, end);
				dl.AddRectFilled(pos, end, selColor);
				dl.PopClipRect();
			}

			// ImGui::SetCursorScreenPos(ImVec2(pos.x + pad.x, barEnd.y + pad.y));

			ImGui::BeginChild("selected tab", ImVec2(0, 0), false,
				ImGuiWindowFlags_AlwaysUseWindowPadding);
			switch(activeTab_) {
				case Tab::overview: drawOverviewUI(draw); break;
				case Tab::memory: drawMemoryUI(draw); break;
				case Tab::commandBuffer: tabs_.cb->draw(draw); break;
				case Tab::resources: tabs_.resources->draw(draw); break;
				default: break;
			}
			ImGui::EndChild();

		} else if(mode_ == Mode::image) {
			auto& iv = standaloneImageViewer();
			iv.display(draw);
		} else {
			dlg_error("invalid mode");
		}
	}

	if(!fullscreen) {
		ImGui::PopStyleVar(1);
		ImGui::PopStyleColor(1);
	}

	ImGui::End();

	ImGui::EndFrame();
	ImGui::Render();
}

void Gui::apiHandleDestroyed(const Handle& handle, VkObjectType type) {
	(void) type;
	assertOwned(dev().mutex);

	auto usesHandle = [&](const Draw& draw) {
		for(auto* usedBuf : draw.usedBuffers) {
			if(usedBuf == &handle) {
				return true;
			}
		}
		for(auto [usedImg, _targetLayout] : draw.usedImages) {
			if(usedImg == &handle) {
				return true;
			}
		}

		return false;
	};

	// Check if we are currently drawing (in another thread) and used
	// one of the destroyed handles
	if(currDraw_ && usesHandle(*currDraw_)) {
		dlg_trace("waiting for draw completion (CPU)");

		currDrawInvalidated_.fetch_add(1);
		std::unique_lock lock(dev().mutex, std::adopt_lock);
		currDrawWait_.wait(lock, [&]{ return currDraw_ == nullptr; });
		dlg_trace(">> wait done");
		auto waitingCount = currDrawInvalidated_.fetch_sub(1);
		if(waitingCount == 1u) {
			currDrawWait_.notify_one();
		}

		// mutex will stay locked
		(void) lock.release();
		dlg_trace(">> continuing");
	}

	// Make sure that all our submissions that use the given handle have
	// finished.
	std::vector<VkFence> fences;
	std::vector<Draw*> draws;
	for(auto& draw : draws_) {
		if(draw->inUse && usesHandle(*draw)) {
			fences.push_back(draw->fence);
			draws.push_back(draw.get());
		}
	}

	if(!fences.empty()) {
		dlg_trace("waiting for draw completion (GPU)");

		VK_CHECK_DEV(dev().dispatch.WaitForFences(dev().handle, u32(fences.size()),
			fences.data(), true, UINT64_MAX), dev());

		for(auto* draw : draws) {
			finishedLocked(*draw);
		}
	}
}

void Gui::memoryResourceInvalidated(const MemoryResource& res) {
	apiHandleDestroyed(res, res.memObjectType);
}

void Gui::activateTab(Tab tab) {
	activeTab_ = tab;
	activateTabCounter_ = 0u;

	// When switching towards the resources tab, make sure to refresh
	// the list of available resources, not showing "<Destroyed>"
	if(tab == Tab::resources) {
		tabs_.resources->firstUpdate_ = true;
	} else if(tab == Tab::commandBuffer &&
			!tabs_.cb->selector().record() &&
			!tabs_.cb->selector().submission()) {
		// when we first swtich to the command tab, select the swapchain
		// by default (if there is any)
		assertNotOwned(dev_->mutex);
		if(auto sc = dev_->swapchain(); sc) {
			tabs_.cb->showSwapchainSubmissions(*sc, true);
		}
	}
}

VkResult Gui::tryRender(Draw& draw, FrameInfo& info) {
	auto cleanupUnfished = [](Draw& draw) {
		for(auto& fcb : draw.onFinish) {
			fcb(draw, false);
		}

		draw.onFinish.clear();
		draw.usedImages.clear();
		draw.usedBuffers.clear();
		draw.usedHookState.reset();
	};

	// clean up finished application submissions
	// not strictly needed here but might make additional information
	// available to command viewer
	{
		std::lock_guard devMutex(dev().mutex);

		dlg_assert(currDrawInvalidated_.load() == 0u);
		dlg_assert(!currDraw_);
		currDraw_ = &draw;

		for(auto it = dev().pending.begin(); it != dev().pending.end();) {
			auto& subm = *it;
			if(auto nit = checkLocked(*subm); nit) {
				it = *nit;
				continue;
			}

			++it; // already increment to next one so we can't miss it
		}
	}

	// draw the ui
	VkCommandBufferBeginInfo cbBegin {};
	cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(dev().dispatch.BeginCommandBuffer(draw.cb, &cbBegin));
	DebugLabel cblbl(dev(), draw.cb, "vil:Gui:draw");

	ensureFontAtlas(draw.cb);

	this->draw(draw, info.fullscreen);
	auto& drawData = *ImGui::GetDrawData();
	this->uploadDraw(draw, drawData);

	auto blurred = false;
	if(blur_.dev && !info.clear) {
		auto& sc = dev().swapchains.get(info.swapchain);
		if(sc.supportsSampling) {
			vil::blur(blur_, draw.cb, info.imageIdx, {}, {});
			blurred = true;
		}
	}

	for(auto& cb : preRender_) {
		cb(draw);
	}
	preRender_.clear();

	if(info.clear) {
		VkImageMemoryBarrier dstBarrier[1] = {};
		dstBarrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		dstBarrier[0].dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dstBarrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		dstBarrier[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		dstBarrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dstBarrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dstBarrier[0].image = info.image;
		dstBarrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		dstBarrier[0].subresourceRange.levelCount = 1;
		dstBarrier[0].subresourceRange.layerCount = 1;
		dev().dispatch.CmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, NULL, 0, NULL,
			1, dstBarrier);
	}

	// optionally blur or clear
	VkRenderPassBeginInfo rpBegin {};
	rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpBegin.renderArea.extent = info.extent;
	rpBegin.renderPass = rp_;
	rpBegin.framebuffer = info.fb;

	VkClearValue clearValues[2] {};
	// clearValues[0] is ignored.
	// our depth attachment, always clear that.
	clearValues[1].depthStencil = {1.f, 0u};

	rpBegin.pClearValues = clearValues;
	rpBegin.clearValueCount = 2u;

	dev().dispatch.CmdBeginRenderPass(draw.cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

	// clearing and then blurring does not make sense
	dlg_assert(!blurred || !info.clear);
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
		auto pcrStages = VK_SHADER_STAGE_VERTEX_BIT |
			VK_SHADER_STAGE_FRAGMENT_BIT |
			VK_SHADER_STAGE_COMPUTE_BIT;
		dev().dispatch.CmdPushConstants(draw.cb, imguiPipeLayout_.vkHandle(),
			pcrStages, 0, sizeof(pcr), pcr);
		dev().dispatch.CmdBindDescriptorSets(draw.cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
			imguiPipeLayout_.vkHandle(), 0u, 1u, &blurDs_.vkHandle(), 0, nullptr);
		VkDeviceSize off = 0u;
		dev().dispatch.CmdBindVertexBuffers(draw.cb, 0u, 1u, &blur_.vertices.buf, &off);
		dev().dispatch.CmdDraw(draw.cb, 6, 1, 0, 0);
	} else if(info.clear) {
		VkClearAttachment clearAtt {};
		clearAtt.clearValue.color = {{0.f, 0.f, 0.f, 1.f}};
		clearAtt.colorAttachment = 0u;
		clearAtt.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

		VkClearRect clearRect {};
		clearRect.rect = {{0u, 0u}, info.extent};
		clearRect.layerCount = 1u;

		dev().dispatch.CmdClearAttachments(draw.cb, 1u, &clearAtt, 1u, &clearRect);
	}

	this->recordDraw(draw, info.extent, info.fb, drawData);

	dev().dispatch.CmdEndRenderPass(draw.cb);

	for(auto& cb : postRender_) {
		cb(draw);
	}
	postRender_.clear();

	dev().dispatch.EndCommandBuffer(draw.cb);

	// == Critical section ==
	// Important we already lock this mutex here since we need to make
	// sure no new submissions are done by application while we process
	// and evaluate the pending submissions
	// NOTE: lock order is important here! First lock device mutex,
	// later on lock queue mutex, that's how we must always do it.
	std::unique_lock devLock(dev().mutex);

	dlg_assert(currDraw_ == &draw);
	currDraw_ = nullptr;

	if(currDrawInvalidated_.load() > 0u) {
		cleanupUnfished(draw);

		// notify all destruction threads that they can carry on
		currDrawWait_.notify_all();

		// now wait for all destruction threads to acknowledge
		// NOTE: might seem useless but this step is important for the
		//   destruction thread waiting predicate, currDraw_ might
		//   not be null anymore until they wake up if we just carry on.
		currDrawWait_.wait(devLock, [&]{
			return currDrawInvalidated_.load() == 0u;
		});

		return VK_INCOMPLETE;
	}

	// == Submit batch ==
	ZoneScopedN("BuildSubmission");

	{
		ZoneScopedN("RecordLocked");

		// cbPre, cbPost
		VkCommandBufferBeginInfo cbBegin {};
		cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(dev().dispatch.BeginCommandBuffer(draw.cbLockedPre, &cbBegin));
		VK_CHECK(dev().dispatch.BeginCommandBuffer(draw.cbLockedPost, &cbBegin));

		if(!draw.usedImages.empty() || !draw.usedBuffers.empty()) {
			ThreadMemScope tms;

			ScopedVector<VkImageMemoryBarrier> imgBarriersPre(tms);
			ScopedVector<VkImageMemoryBarrier> imgBarriersPost(tms);

			imgBarriersPre.reserve(2 * draw.usedImages.size());
			imgBarriersPost.reserve(2 * draw.usedImages.size());

			for(auto& [img, targetLayout] : draw.usedImages) {
				// undefined doesn't make sense, whoever added it to usedImages
				// can't know which layout the image is in and can't use it
				// without knowing that.
				dlg_assert_or(targetLayout != VK_IMAGE_LAYOUT_UNDEFINED, continue);

				for(auto& subres : img->pendingLayoutLocked()) {
					// dlg_trace("mip {}:{} layer {}:{} layout {}",
					// 	subres.range.baseMipLevel,
					// 	subres.range.levelCount,
					// 	subres.range.baseArrayLayer,
					// 	subres.range.layerCount,
					// 	vk::name(subres.layout));

					auto& barrierPre = imgBarriersPre.emplace_back();
					barrierPre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
					barrierPre.image = img->handle;
					barrierPre.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
					barrierPre.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
					barrierPre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					barrierPre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					barrierPre.oldLayout = subres.layout;
					barrierPre.newLayout = targetLayout;
					barrierPre.subresourceRange = subres.range;

					// we can't (and don't have to) transition back to undefined
					// layout.
					if(subres.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
						continue;
					}

					// TODO: fix this case, not so sure about handling it
					dlg_assert_or(subres.layout != VK_IMAGE_LAYOUT_PREINITIALIZED,
						continue);

					auto& barrierPost = imgBarriersPost.emplace_back();
					barrierPost.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
					barrierPost.image = img->handle;
					barrierPost.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
					barrierPost.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
					barrierPost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					barrierPost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					barrierPost.oldLayout = targetLayout;
					barrierPost.newLayout = subres.layout;
					barrierPost.subresourceRange = subres.range;
				}
			}

			// Insert general memory barrier to make sure we don't interfer
			// with application on this queue.
			auto allAccess = VK_ACCESS_MEMORY_WRITE_BIT |
				VK_ACCESS_MEMORY_READ_BIT |
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
				VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
				VK_ACCESS_SHADER_READ_BIT |
				VK_ACCESS_SHADER_WRITE_BIT;
			VkMemoryBarrier memb {};
			memb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memb.srcAccessMask = allAccess;
			memb.dstAccessMask = allAccess;

			dev().dispatch.CmdPipelineBarrier(draw.cbLockedPre,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0, 1, &memb, 0, nullptr,
				imgBarriersPre.size(), imgBarriersPre.data());

			dev().dispatch.CmdPipelineBarrier(draw.cbLockedPost,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0, 1, &memb, 0, nullptr,
				imgBarriersPost.size(), imgBarriersPost.data());
		}

		dev().dispatch.EndCommandBuffer(draw.cbLockedPre);
		dev().dispatch.EndCommandBuffer(draw.cbLockedPost);
	}

	VkSubmitInfo submitInfo {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	const auto cbs = std::array{draw.cbLockedPre, draw.cb, draw.cbLockedPost};
	submitInfo.commandBufferCount = cbs.size();
	submitInfo.pCommandBuffers = cbs.data();

	// NOTE: could alternatively retrieve all submissions via
	// handle->refRecords->cb->pending (and handle->descriptos->...)
	// but this should be faster, there are usually only a small
	// number of pending submissions while there might be more recordings
	// referencing a handle.

	waitSemaphores_.clear();
	waitSemaphores_.insert(waitSemaphores_.end(),
		info.waitSemaphores.begin(), info.waitSemaphores.end());

	waitStages_.resize(info.waitSemaphores.size());
	std::fill_n(waitStages_.begin(), info.waitSemaphores.size(),
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

	signalSemaphores_.clear();
	signalSemaphores_.emplace_back(draw.presentSemaphore);

	if(dev().doFullSync) {
		addFullSync(draw, submitInfo);
	} else {
		auto res = addLegacySync(draw, submitInfo);
		if(res != VK_SUCCESS) {
			dlg_assert(res != VK_INCOMPLETE);
			return res;
		}
	}

	VkResult res;

	{
		ZoneScopedN("dispatch.QueueSubmit");

		// PERF: when using timeline semaphores we don't need a
		// fence and can just use the timeline semaphore
		std::lock_guard queueLock(dev().queueMutex);
		res = dev().dispatch.QueueSubmit(usedQueue().handle,
			1u, &submitInfo, draw.fence);
	}

	if(res != VK_SUCCESS) {
		dlg_error("vkQueueSubmit error: {}", vk::name(res));
		dlg_assert(res != VK_INCOMPLETE);
		cleanupUnfished(draw);
		return res;
	}

	if(dev().timelineSemaphores) {
		usedQueue().lastLayerSubmission = draw.lastSubmissionID;
	}

	draw.inUse = true;
	draw.futureSemaphoreSignaled = true;
	draw.futureSemaphoreUsed = false;
	lastDraw_ = &draw;

	return VK_SUCCESS;
}

VkResult Gui::renderFrame(FrameInfo& info) {
	ZoneScoped;
	FrameMark;

	makeImGuiCurrent();
	Draw* foundDraw {};

	// find a free draw object
	{
		std::lock_guard devMutex(dev().mutex);

		for(auto& draw : draws_) {
			if(!draw->inUse) {
				foundDraw = draw.get();
				break;
			}

			if(dev().dispatch.GetFenceStatus(dev().handle, draw->fence) == VK_SUCCESS) {
				finishedLocked(*draw);
				foundDraw = draw.get();
				break;
			}
		}

		if(!foundDraw) {
			auto draw = std::make_unique<Draw>(*this, commandPool_);
			foundDraw = draws_.emplace_back(std::move(draw)).get();
		}
	}

	auto& draw = *foundDraw;
	draw.usedImages.clear();
	draw.usedBuffers.clear();
	foundDraw->lastUsed = ++drawCounter_;

	if(blur_.dev && !info.clear) {
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
				imgInfo.sampler = dev().linearSampler;

				VkWriteDescriptorSet write {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.pImageInfo = &imgInfo;
				write.descriptorCount = 1u;
				write.dstArrayElement = 0u;
				write.dstSet = blurDs_.vkHandle();
				write.dstBinding = 0u;
				write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

				dev().dispatch.UpdateDescriptorSets(dev().handle, 1u, &write, 0u, nullptr);
			}
		}
	}

	auto& io = ImGui::GetIO();
	io.DisplaySize.x = info.extent.width;
	io.DisplaySize.y = info.extent.height;

	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;
	auto now = Clock::now();
	auto diff = now - lastFrame_;
	lastFrame_ = now;
	dt_ = std::chrono::duration_cast<Secf>(diff).count();
	if(dt_ > 0.f) {
		ImGui::GetIO().DeltaTime = dt_;
	}

	// process events
	std::vector<Event> movedEvents;
	{
		std::lock_guard lock(eventMutex_);
		movedEvents = std::move(events_);
	}

	for(auto& event : movedEvents) {
		switch(event.type) {
			case Event::Type::input:
				io.AddInputCharactersUTF8(event.input.data());
				break;
			case Event::Type::input16:
				io.AddInputCharacterUTF16(event.input16);
				break;
			case Event::Type::key:
				io.AddKeyEvent(event.key, event.b);
				break;
			case Event::Type::mousePos:
				io.AddMousePosEvent(event.vec2f.x, event.vec2f.y);
				break;
			case Event::Type::mouseButton:
				io.AddMouseButtonEvent(event.button, event.b);
				break;
			case Event::Type::mouseWheel:
				io.AddMouseWheelEvent(event.vec2f.x, event.vec2f.y);
				break;
		}
	}

	VkResult res = VK_INCOMPLETE;
	while(true) {
		res = tryRender(draw, info);
		if(res == VK_SUCCESS) {
			break;
		}

		// error case
		if(res != VK_INCOMPLETE) {
			return res;
		}

		dlg_info("re-trying rendering after mid-draw invalidation");
	}

	// upadte event info
	{
		std::lock_guard lock(eventMutex_);
		eventCaptureKeyboard_ = io.WantCaptureKeyboard;
		eventCaptureMouse_ = io.WantCaptureMouse;
		eventWantTextInput_ = io.WantTextInput;
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

	if(res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
		dlg_error("vkQueuePresentKHR error: {}", vk::name(res));

		// TODO: would have to reset foundDraw->presentSemaphore here right?
		// TODO: not sure how to handle this the best
		VK_CHECK_DEV(dev().dispatch.WaitForFences(dev().handle, 1,
			&foundDraw->fence, true, UINT64_MAX), dev());

		std::lock_guard lock(dev().mutex);
		finishedLocked(*foundDraw);

		return res;
	}

	return VK_SUCCESS;
}

VkResult Gui::addLegacySync(Draw& draw, VkSubmitInfo& submitInfo) {
	std::vector<Submission*> waitSubmissions;
	for(auto& pending : reversed(dev().pending)) {
		// When the pending submission was submitted to the guiQueue
		// we don't need to sync via semaphore, the pipeline barrier is enough
		if(pending->queue == dev().gfxQueue) {
			continue;
		}

		// We only need to chain the last submission on each
		// queue as semaphores guarantee that everything before
		// has finished. Since dev().pending is ordered (by submission
		// order) we just take the first submission per queue
		auto found = false;
		for(auto& sub : waitSubmissions) {
			if(sub->parent->queue == pending->queue) {
				dlg_assert(sub->parent->globalSubmitID > pending->globalSubmitID);
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

	if(dev().timelineSemaphores) {
		dlg_assert(dev().resetSemaphores.empty());

		tsInfo_.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		tsInfo_.pNext = submitInfo.pNext;

		// wait
		waitValues_.resize(waitSemaphores_.size()); // initial ones, ignored
		for(auto* sub : waitSubmissions) {
			// PERF: guess we could do better
			waitStages_.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			waitSemaphores_.push_back(sub->parent->queue->submissionSemaphore);
			waitValues_.push_back(sub->queueSubmitID);
		}

		dlg_assert(waitValues_.size() == waitSemaphores_.size());
		tsInfo_.waitSemaphoreValueCount = u32(waitValues_.size());
		tsInfo_.pWaitSemaphoreValues = waitValues_.data();

		// signal
		signalSemaphores_.push_back(usedQueue().submissionSemaphore);
		draw.lastSubmissionID = ++usedQueue().submissionCounter;

		// signalValues[0] is uninitialized by design, should be
		// ignored by driver as signalSemaphores[0] is binary, we need
		// that since present semaphores cannot be timelined
		signalValues_.resize(2);
		signalValues_[1] = draw.lastSubmissionID;

		tsInfo_.signalSemaphoreValueCount = signalValues_.size();
		tsInfo_.pSignalSemaphoreValues = signalValues_.data();

		submitInfo.pNext = &tsInfo_;
	} else {
		// add dev.resetSemaphores while we are at it
		for(auto sem : dev().resetSemaphores) {
			waitSemaphores_.push_back(sem);
			waitStages_.push_back(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
			draw.waitedUpon.push_back(sem);
		}

		dev().resetSemaphores.clear();

		// When this draw's futureSemaphore wasn't used, make sure
		// to reset it before we signal it again. This shouldn't acutally
		// cause an additional wait, we already wait for the last draw.
		if(!draw.futureSemaphoreUsed && draw.futureSemaphoreSignaled) {
			// This is a bit messy.
			// So, 'futureSemaphore' was signaled by us but never consumed
			// (happens when we just didn't access anything the
			// application used since then). We have to reset the
			// semaphore.
			// Initially, we simply waited on it via
			// submitInfo.pWaitSemaphores but no one knows if this
			// is valid or not, the spec is too vague.
			// Since this lead to a deadlock with anv, mesa 20.3, 21.1,
			// we just swap it out with a pool semaphore now.
			// We can't recreate it without waiting on draw.fence
			// which we want to avoid.
			dev().resetSemaphores.push_back(draw.futureSemaphore);
			draw.futureSemaphore = getSemaphoreFromPoolLocked(dev());

			draw.futureSemaphoreUsed = false;
			draw.futureSemaphoreSignaled = false;
		}

		signalSemaphores_.push_back(draw.futureSemaphore);

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
			waitStages_.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			waitSemaphores_.push_back(sem);
			draw.waitedUpon.push_back(sem);
		}
	}

	dlg_assert(waitStages_.size() == waitSemaphores_.size());

	submitInfo.signalSemaphoreCount = u32(signalSemaphores_.size());
	submitInfo.pSignalSemaphores = signalSemaphores_.data();
	submitInfo.pWaitDstStageMask = waitStages_.data();
	submitInfo.waitSemaphoreCount = u32(waitSemaphores_.size());
	submitInfo.pWaitSemaphores = waitSemaphores_.data();

	return VK_SUCCESS;
}

void Gui::addFullSync(Draw& draw, VkSubmitInfo& submitInfo) {
	dlg_assert(dev().timelineSemaphores);

	// tsInfo_
	tsInfo_.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	tsInfo_.pNext = submitInfo.pNext;

	// wait
	waitValues_.resize(waitSemaphores_.size()); // initial ones, ignored

	// when we used any application resources we sync with *all*
	// pending submissions.
	if(!draw.usedImages.empty() || !draw.usedBuffers.empty()) {
		for(auto& pqueue : dev().queues) {
			auto& queue = *pqueue;
			if(&queue == &usedQueue()) {
				continue;
			}

			u64 finishedID;
			dev().dispatch.GetSemaphoreCounterValue(dev().handle,
				queue.submissionSemaphore, &finishedID);

			// no pending submissions on this queue
			if(finishedID == queue.submissionCounter) {
				continue;
			}

			dlg_assert(finishedID < queue.submissionCounter);
			waitValues_.push_back(queue.submissionCounter);
			waitSemaphores_.push_back(queue.submissionSemaphore);
			waitStages_.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}
	}

	dlg_assert(waitValues_.size() == waitSemaphores_.size());
	tsInfo_.waitSemaphoreValueCount = u32(waitValues_.size());
	tsInfo_.pWaitSemaphoreValues = waitValues_.data();

	// signal
	signalSemaphores_.push_back(usedQueue().submissionSemaphore);
	draw.lastSubmissionID = ++usedQueue().submissionCounter;

	// signalValues[0] is uninitialized by design, should be
	// ignored by driver as signalSemaphores[0] is binary, we need
	// that since present semaphores cannot be timelined
	signalValues_.resize(2);
	signalValues_[1] = draw.lastSubmissionID;

	tsInfo_.signalSemaphoreValueCount = signalValues_.size();
	tsInfo_.pSignalSemaphoreValues = signalValues_.data();

	submitInfo.pNext = &tsInfo_;

	// build submitInfo
	dlg_assert(waitStages_.size() == waitSemaphores_.size());

	submitInfo.signalSemaphoreCount = u32(signalSemaphores_.size());
	submitInfo.pSignalSemaphores = signalSemaphores_.data();
	submitInfo.pWaitDstStageMask = waitStages_.data();
	submitInfo.waitSemaphoreCount = u32(waitSemaphores_.size());
	submitInfo.pWaitSemaphores = waitSemaphores_.data();
}

void Gui::waitForDraws() {
	std::vector<VkFence> fences;
	for(auto& draw : draws_) {
		if(draw->inUse) {
			fences.push_back(draw->fence);
		}
	}

	if(!fences.empty()) {
		VK_CHECK_DEV(dev().dispatch.WaitForFences(dev().handle,
			u32(fences.size()), fences.data(), true, UINT64_MAX), dev());
		// we can't reset the draws here
	}
}

void Gui::makeImGuiCurrent() {
	ImGui::SetCurrentContext(imgui_);
}

void Gui::selectResource(Handle& handle, VkObjectType objectType, bool activateTab) {
	tabs_.resources->select(handle, objectType);

	if(activateTab) {
		this->activateTab(Tab::resources);
	}
}

Draw* Gui::latestPendingDrawSyncLocked(SubmissionBatch& batch) {
	Draw* ret {};
	for(auto& draw : draws_) {
		if(!draw->inUse || dev().dispatch.GetFenceStatus(dev().handle, draw->fence) == VK_SUCCESS) {
			continue;
		}

		if((!ret || draw->lastUsed > ret->lastUsed) && !needsSyncLocked(batch, *draw).empty()) {
			ret = draw.get();
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

	for(auto& cb : draw.onFinish) {
		cb(draw, true);
	}

	draw.onFinish.clear();
	draw.waitedUpon.clear();
	draw.usedImages.clear();
	draw.usedBuffers.clear();
	draw.usedHookState.reset();

	VK_CHECK_DEV(dev().dispatch.ResetFences(dev().handle, 1, &draw.fence), dev());

	draw.inUse = false;
}

void Gui::addPreRender(Recorder rec) {
	preRender_.emplace_back(std::move(rec));
}

void Gui::addPostRender(Recorder rec) {
	postRender_.emplace_back(std::move(rec));
}

Queue& Gui::usedQueue() const {
	return *dev().gfxQueue;
}

void Gui::visible(bool newVisible) {
	visible_ = newVisible;

	if(!newVisible) {
		auto& hook = *dev().commandHook;
		hook.freeze.store(true);
	}
}

void Gui::updateColorFormat(VkFormat newColorFormat) {
	if(colorFormat_ == newColorFormat) {
		return;
	}

	colorFormat_ = newColorFormat;
	initRenderStuff();
}

bool Gui::addKeyEvent(ImGuiKey key, bool down) {
	Event ev {};
	ev.type = Event::Type::key;
	ev.key = key;
	ev.b = down;

	std::lock_guard lock(eventMutex_);
	events_.push_back(ev);

	return eventCaptureKeyboard_;
}
void Gui::addMousePosEvent(Vec2f pos) {
	Event ev {};
	ev.type = Event::Type::mousePos;
	ev.vec2f = pos;

	std::lock_guard lock(eventMutex_);
	events_.push_back(ev);
}
bool Gui::addMouseButtonEvent(int button, bool down) {
	Event ev {};
	ev.type = Event::Type::mouseButton;
	ev.button = button;
	ev.b = down;

	std::lock_guard lock(eventMutex_);
	events_.push_back(ev);

	return eventCaptureMouse_;
}
bool Gui::addMouseWheelEvent(Vec2f dir) {
	Event ev {};
	ev.type = Event::Type::mouseWheel;
	ev.vec2f = dir;

	std::lock_guard lock(eventMutex_);
	events_.push_back(ev);

	return eventCaptureMouse_;
}
bool Gui::addInputEvent(std::string input) {
	Event ev {};
	ev.type = Event::Type::input;
	ev.input = std::move(input);

	std::lock_guard lock(eventMutex_);
	events_.push_back(ev);

	return eventCaptureKeyboard_ || eventWantTextInput_;
}

bool Gui::addInputEvent(unsigned short input16) {
	Event ev {};
	ev.type = Event::Type::input16;
	ev.input16 = input16;

	std::lock_guard lock(eventMutex_);
	events_.push_back(ev);

	return eventCaptureKeyboard_ || eventWantTextInput_;
}

vku::DynDs Gui::allocDs(const vku::DynDsLayout& layout, StringParam name) {
	VkDescriptorSetAllocateInfo dsai {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = dev_->dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &layout.vkHandle();

	VkDescriptorSet ds;
	VK_CHECK(dev_->dispatch.AllocateDescriptorSets(dev_->handle, &dsai, &ds));

	auto ret = vku::DynDs(dev_->dsPool, layout, ds);

	if(!name.empty()) {
		nameHandle(ret, name);
	}

	return ret;
}

// util
// Copied from ImGui::SmallButton
bool inlineButton(const char* label, const ImVec2& size_arg = ImVec2(0, 0), ImGuiButtonFlags flags = 0) {
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiID id = window->GetID(label);
	const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

	ImVec2 pos = window->DC.CursorPos;

	// SmallButton-like
	ImVec2 padding = ImVec2(0, 0);
	// const ImGuiStyle& style = g.Style;
	// ImVec2 padding = style.FramePadding;
	// padding.y = 0;

	ImVec2 size = ImGui::CalcItemSize(size_arg, label_size.x + padding.x * 2.0f, label_size.y + padding.y * 2.0f);

	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(size, padding.y);
	if (!ImGui::ItemAdd(bb, id))
		return false;

	if (g.LastItemData.InFlags & ImGuiItemFlags_ButtonRepeat)
		flags |= ImGuiButtonFlags_Repeat;

	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);

	// Render
	ImGui::RenderNavHighlight(bb, id);

	if (g.LogEnabled) {
		ImGui::LogSetNextTextDecoration("[", "]");
	}

	const ImU32 col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive :
			hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
	ImGui::PushStyleColor(ImGuiCol_Text, col);
	ImGui::RenderText(bb.Min + padding, label, NULL);
	ImGui::PopStyleColor(1);

	return pressed;
}

void refButton(Gui& gui, Handle& handle, VkObjectType objectType) {
	// We need the PushID/PopID since there may be multiple
	// ref buttons with the same label (e.g. for unnamed handles)
	constexpr auto showType = true;
	ImGui::PushID(&handle);

	ImGui::PushStyleColor(ImGuiCol_Button, gui.inlineButtonCol);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, gui.inlineButtonColHovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, gui.inlineButtonColActive);

	if(inlineButton(name(handle, objectType, showType).c_str())) {
		gui.selectResource(handle, objectType);
	}

	ImGui::PopStyleColor(3);

	ImGui::PopID();
}

void pushDisabled(bool disabled) {
	if(disabled) {
		ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
	}
}

void popDisabled(bool disabled) {
	if(disabled) {
		ImGui::PopStyleVar();
		ImGui::PopItemFlag();
	}
}

// TODO: kinda ugly, cleanup
std::vector<unsigned> initImguiKeymap() {
	std::vector<unsigned> ret;
	ret.resize(1024);

	// TODO: add other keys
	ret[VilKeyA] = ImGuiKey_A;
	ret[VilKeyB] = ImGuiKey_B;
	ret[VilKeyC] = ImGuiKey_C;
	ret[VilKeyD] = ImGuiKey_D;
	ret[VilKeyE] = ImGuiKey_E;
	ret[VilKeyF] = ImGuiKey_F;
	ret[VilKeyG] = ImGuiKey_G;
	ret[VilKeyH] = ImGuiKey_H;
	ret[VilKeyI] = ImGuiKey_I;
	ret[VilKeyJ] = ImGuiKey_J;
	ret[VilKeyK] = ImGuiKey_K;
	ret[VilKeyL] = ImGuiKey_L;
	ret[VilKeyM] = ImGuiKey_M;
	ret[VilKeyN] = ImGuiKey_N;
	ret[VilKeyO] = ImGuiKey_O;
	ret[VilKeyP] = ImGuiKey_P;
	ret[VilKeyQ] = ImGuiKey_Q;
	ret[VilKeyR] = ImGuiKey_R;
	ret[VilKeyS] = ImGuiKey_S;
	ret[VilKeyT] = ImGuiKey_T;
	ret[VilKeyU] = ImGuiKey_U;
	ret[VilKeyV] = ImGuiKey_V;
	ret[VilKeyW] = ImGuiKey_W;
	ret[VilKeyX] = ImGuiKey_X;
	ret[VilKeyY] = ImGuiKey_Y;
	ret[VilKeyZ] = ImGuiKey_Z;

	ret[VilKey0] = ImGuiKey_0;
	ret[VilKey1] = ImGuiKey_1;
	ret[VilKey2] = ImGuiKey_2;
	ret[VilKey3] = ImGuiKey_3;
	ret[VilKey4] = ImGuiKey_4;
	ret[VilKey5] = ImGuiKey_5;
	ret[VilKey6] = ImGuiKey_6;
	ret[VilKey7] = ImGuiKey_7;
	ret[VilKey8] = ImGuiKey_8;
	ret[VilKey9] = ImGuiKey_9;

	ret[VilKeyMinus] = ImGuiKey_Minus;
	ret[VilKeyEquals] = ImGuiKey_Equal;
	ret[VilKeyTab] = ImGuiKey_Tab;
	ret[VilKeyBackspace] = ImGuiKey_Backspace;

	ret[VilKeyLeftbrace] = ImGuiKey_LeftBracket;
	ret[VilKeyRightbrace] = ImGuiKey_RightBracket;
	ret[VilKeyEnter] = ImGuiKey_Enter;
	ret[VilKeyLeftctrl] = ImGuiKey_LeftCtrl;

	ret[VilKeySemicolon] = ImGuiKey_Semicolon;
	ret[VilKeyApostrophe] = ImGuiKey_Apostrophe;
	ret[VilKeyGrave] = ImGuiKey_GraveAccent;
	ret[VilKeyLeftshift] = ImGuiKey_LeftShift;
	ret[VilKeyBackslash] = ImGuiKey_Backslash;

	ret[VilKeyComma] = ImGuiKey_Comma;
	ret[VilKeyPeriod] = ImGuiKey_Period;
	ret[VilKeySlash] = ImGuiKey_Slash;
	ret[VilKeyRightshift] = ImGuiKey_RightShift;
	ret[VilKeyKpmultiply] = ImGuiKey_KeypadMultiply;
	ret[VilKeyLeftalt] = ImGuiKey_LeftAlt;
	ret[VilKeySpace] = ImGuiKey_Space;
	ret[VilKeyCapslock] = ImGuiKey_CapsLock;

	ret[VilKeyF1] = ImGuiKey_F1;
	ret[VilKeyF2] = ImGuiKey_F2;
	ret[VilKeyF3] = ImGuiKey_F3;
	ret[VilKeyF4] = ImGuiKey_F4;
	ret[VilKeyF5] = ImGuiKey_F5;
	ret[VilKeyF6] = ImGuiKey_F6;
	ret[VilKeyF7] = ImGuiKey_F7;
	ret[VilKeyF8] = ImGuiKey_F8;
	ret[VilKeyF9] = ImGuiKey_F9;
	ret[VilKeyF10] = ImGuiKey_F10;
	ret[VilKeyF11] = ImGuiKey_F11;
	ret[VilKeyF12] = ImGuiKey_F12;

	ret[VilKeyNumlock] = ImGuiKey_NumLock;
	ret[VilKeyScrollock] = ImGuiKey_ScrollLock;
	ret[VilKeyKpminus] = ImGuiKey_KeypadSubtract;
	ret[VilKeyKpplus] = ImGuiKey_KeypadAdd;
	// kpmult

	ret[VilKeyKp0] = ImGuiKey_Keypad0;
	ret[VilKeyKp1] = ImGuiKey_Keypad1;
	ret[VilKeyKp2] = ImGuiKey_Keypad2;
	ret[VilKeyKp3] = ImGuiKey_Keypad3;
	ret[VilKeyKp4] = ImGuiKey_Keypad4;
	ret[VilKeyKp5] = ImGuiKey_Keypad5;
	ret[VilKeyKp6] = ImGuiKey_Keypad6;
	ret[VilKeyKp7] = ImGuiKey_Keypad7;
	ret[VilKeyKp8] = ImGuiKey_Keypad8;
	ret[VilKeyKp9] = ImGuiKey_Keypad9;

	ret[VilKeyLeft] = ImGuiKey_LeftArrow;
	ret[VilKeyDown] = ImGuiKey_DownArrow;
	ret[VilKeyRight] = ImGuiKey_RightArrow;
	ret[VilKeyUp] = ImGuiKey_UpArrow;
	ret[VilKeyPagedown] = ImGuiKey_PageDown;
	ret[VilKeyPageup] = ImGuiKey_PageUp;
	ret[VilKeyInsert] = ImGuiKey_Insert;
	ret[VilKeyDel] = ImGuiKey_Delete;

	return ret;
}

ImGuiKey keyToImGui(unsigned key) {
	static auto map = initImguiKeymap();
	dlg_assert_or(key < map.size(), return ImGuiKey_None);
	return map[key];
}

ImageViewer& Gui::standaloneImageViewer() {
	if(!tabs_.imageViewer) {
		tabs_.imageViewer = std::make_unique<ImageViewer>();
		tabs_.imageViewer->init(*this);
	}
	return *tabs_.imageViewer;
}

} // namespace vil
