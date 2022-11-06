#include <device.hpp>
#include <gui/gui.hpp>
#include <nytl/span.hpp>
#include <vkutil/handles.hpp>
#include <array>

// shaders
#include <gui.frag.spv.h>
#include <gui.vert.spv.h>
#include <imagebg.vert.spv.h>
#include <imagebg.frag.spv.h>

#include <image.frag.1DArray.spv.h>
#include <image.frag.u1DArray.spv.h>
#include <image.frag.i1DArray.spv.h>
#include <image.frag.2DArray.spv.h>
#include <image.frag.u2DArray.spv.h>
#include <image.frag.i2DArray.spv.h>
#include <image.frag.3D.spv.h>
#include <image.frag.u3D.spv.h>
#include <image.frag.i3D.spv.h>

#include <readTex.comp.1DArray.spv.h>
#include <readTex.comp.u1DArray.spv.h>
#include <readTex.comp.i1DArray.spv.h>
#include <readTex.comp.2DArray.spv.h>
#include <readTex.comp.u2DArray.spv.h>
#include <readTex.comp.i2DArray.spv.h>
#include <readTex.comp.3D.spv.h>
#include <readTex.comp.u3D.spv.h>
#include <readTex.comp.i3D.spv.h>

#include <minmax.comp.1DArray.spv.h>
#include <minmax.comp.u1DArray.spv.h>
#include <minmax.comp.i1DArray.spv.h>
#include <minmax.comp.2DArray.spv.h>
#include <minmax.comp.u2DArray.spv.h>
#include <minmax.comp.i2DArray.spv.h>
#include <minmax.comp.3D.spv.h>
#include <minmax.comp.u3D.spv.h>
#include <minmax.comp.i3D.spv.h>

#include <histogram.comp.1DArray.spv.h>
#include <histogram.comp.u1DArray.spv.h>
#include <histogram.comp.i1DArray.spv.h>
#include <histogram.comp.2DArray.spv.h>
#include <histogram.comp.u2DArray.spv.h>
#include <histogram.comp.i2DArray.spv.h>
#include <histogram.comp.3D.spv.h>
#include <histogram.comp.u3D.spv.h>
#include <histogram.comp.i3D.spv.h>

#include <histogramMax.comp.spv.h>
#include <histogramPost.comp.spv.h>
#include <histogramPrepare.comp.spv.h>
#include <histogram.vert.spv.h>
#include <histogram.frag.spv.h>

namespace vil {

void initPipes(Device& dev,
		VkRenderPass rp, VkPipelineLayout renderPipeLayout,
		VkPipelineLayout imgOpPipeLayout,
		VkPipelineLayout histogramPipeLayout,
		Gui::Pipelines& dstPipes, bool manualSRGB) {
	std::vector<vku::ShaderModule> modules;
	auto createShaderMod = [&](span<const u32> spv) {
		return modules.emplace_back(dev, spv).vkHandle();
	};

	auto vertModule = createShaderMod(gui_vert_spv_data);

	VkSpecializationMapEntry manualSRGBEntry {};
	manualSRGBEntry.size = 4u;
	manualSRGBEntry.offset = 0u;
	manualSRGBEntry.constantID = 0u;

	u32 uManualSRGB = manualSRGB ? 1u : 0u;
	VkSpecializationInfo srgbSpec {};
	srgbSpec.dataSize = 4u;
	srgbSpec.pData = &uManualSRGB;
	srgbSpec.mapEntryCount = 1u;
	srgbSpec.pMapEntries = &manualSRGBEntry;

	auto initStages = [&](span<const u32> fragSpv) {
		VkShaderModule fragModule = createShaderMod(fragSpv);

		std::array<VkPipelineShaderStageCreateInfo, 2> ret {};
		ret[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ret[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		ret[0].module = vertModule;
		ret[0].pName = "main";

		ret[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ret[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		ret[1].module = fragModule;
		ret[1].pName = "main";
		ret[1].pSpecializationInfo = &srgbSpec;

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
	guiGpi.layout = renderPipeLayout;
	guiGpi.renderPass = rp;
	guiGpi.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

	VkGraphicsPipelineCreateInfo imgGpi = guiGpi;
	imgGpi.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
	imgGpi.basePipelineIndex = 0u;
	imgGpi.pStages = uimage1DStages.data();
	gpis.push_back(imgGpi);

	auto addImGpi = [&](auto& stages) {
		VkGraphicsPipelineCreateInfo gpi = imgGpi;
		gpi.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
		gpi.basePipelineIndex = 1u;
		gpi.pStages = stages.data();
		gpis.push_back(gpi);
	};

	addImGpi(uimage2DStages);
	addImGpi(uimage3DStages);

	addImGpi(iimage1DStages);
	addImGpi(iimage2DStages);
	addImGpi(iimage3DStages);

	addImGpi(image1DStages);
	addImGpi(image2DStages);
	addImGpi(image3DStages);

	// imageBg pipe
	auto imageBgVertModule = createShaderMod(imagebg_vert_spv_data);
	auto imageBgFragModule = createShaderMod(imagebg_frag_spv_data);

	std::array<VkPipelineShaderStageCreateInfo, 2> bgStages {};
	bgStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	bgStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	bgStages[0].module = imageBgVertModule;
	bgStages[0].pName = "main";

	bgStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	bgStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	bgStages[1].module = imageBgFragModule;
	bgStages[1].pName = "main";
	bgStages[1].pSpecializationInfo = &srgbSpec;

	auto imgBgGpi = imgGpi;
	auto imgBgVertInfo = *imgBgGpi.pVertexInputState;
	auto imgBgAssembly = *imgBgGpi.pInputAssemblyState;
	imgBgVertInfo.vertexAttributeDescriptionCount = 0u;
	imgBgVertInfo.vertexBindingDescriptionCount = 0u;
	imgBgGpi.pVertexInputState = &imgBgVertInfo;
	imgBgGpi.pStages = bgStages.data();
	imgBgAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	imgBgGpi.pInputAssemblyState = &imgBgAssembly;
	gpis.push_back(imgBgGpi);

	// histogram pipe
	auto histVertMod = createShaderMod(histogram_vert_spv_data);
	auto histFragMod = createShaderMod(histogram_frag_spv_data);

	std::array<VkPipelineShaderStageCreateInfo, 2> histStages {};
	histStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	histStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	histStages[0].module = histVertMod;
	histStages[0].pName = "main";

	histStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	histStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	histStages[1].module = histFragMod;
	histStages[1].pName = "main";
	histStages[1].pSpecializationInfo = &srgbSpec;

	auto histGpi = imgBgGpi;
	// already done for imgBgGpi
	// auto histVertInfo = *histGpi.pVertexInputState;
	// auto histAssembly = *histGpi.pInputAssemblyState;
	// histVertInfo.vertexAttributeDescriptionCount = 0u;
	// histVertInfo.vertexBindingDescriptionCount = 0u;
	// histGpi.pVertexInputState = &histVertInfo;
	// histAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	// histGpi.pInputAssemblyState = &histAssembly;
	histGpi.pStages = histStages.data();
	histGpi.layout = histogramPipeLayout;
	gpis.push_back(histGpi);

	// init pipes
	VkPipeline pipes[12];
	dlg_assert(gpis.size() == sizeof(pipes) / sizeof(pipes[0]));

	VK_CHECK(dev.dispatch.CreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, u32(gpis.size()), gpis.data(), nullptr, pipes));

	dstPipes.gui = pipes[0];
	nameHandle(dev, dstPipes.gui, "Gui:pipeGui");

	for(auto i = 0u; i < ShaderImageType::count; ++i) {
		dstPipes.image[i] = pipes[1 + i];
		auto name = dlg::format("Gui:pipeImage[{}]", i);
		nameHandle(dev, dstPipes.image[i], name.c_str());
	}

	dstPipes.imageBg = pipes[10];
	nameHandle(dev, dstPipes.imageBg, "Gui:pipeImageBg");

	dstPipes.histogramRender = pipes[11];
	nameHandle(dev, dstPipes.histogramRender, "Gui:histogramRender");

	// init compute pipelines
	std::vector<VkComputePipelineCreateInfo> cpis {};

	auto addCpi = [&](VkPipelineLayout layout, span<const u32> spv) {
		auto mod = createShaderMod(spv);
		auto& cpi = cpis.emplace_back();
		if(cpis.size() > 1u) {
			cpi.basePipelineIndex = 0u;
		}

		cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		cpi.layout = layout;
		cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
	};

	// readTex
	struct PipeCreation {
		std::array<VkPipeline, ShaderImageType::count>& dst;
		VkPipelineLayout layout;
		std::array<span<const u32>, ShaderImageType::count> spvs;
	};

	auto creations = std::array {
		PipeCreation {
			dstPipes.readTex, imgOpPipeLayout, {
				readTex_comp_u1DArray_spv_data,
				readTex_comp_u2DArray_spv_data,
				readTex_comp_u3D_spv_data,
				readTex_comp_i1DArray_spv_data,
				readTex_comp_i2DArray_spv_data,
				readTex_comp_i3D_spv_data,
				readTex_comp_1DArray_spv_data,
				readTex_comp_2DArray_spv_data,
				readTex_comp_3D_spv_data,
			}
		},
		PipeCreation {
			dstPipes.histogramTex, imgOpPipeLayout, {
				histogram_comp_u1DArray_spv_data,
				histogram_comp_u2DArray_spv_data,
				histogram_comp_u3D_spv_data,
				histogram_comp_i1DArray_spv_data,
				histogram_comp_i2DArray_spv_data,
				histogram_comp_i3D_spv_data,
				histogram_comp_1DArray_spv_data,
				histogram_comp_2DArray_spv_data,
				histogram_comp_3D_spv_data,
			}
		},
		PipeCreation {
			dstPipes.minMaxTex, imgOpPipeLayout, {
				minmax_comp_u1DArray_spv_data,
				minmax_comp_u2DArray_spv_data,
				minmax_comp_u3D_spv_data,
				minmax_comp_i1DArray_spv_data,
				minmax_comp_i2DArray_spv_data,
				minmax_comp_i3D_spv_data,
				minmax_comp_1DArray_spv_data,
				minmax_comp_2DArray_spv_data,
				minmax_comp_3D_spv_data,
			}
		},
	};

	for(auto& creation : creations) {
		for(auto& spv : creation.spvs) {
			addCpi(creation.layout, spv);
		}

		VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, VK_NULL_HANDLE,
			u32(cpis.size()), cpis.data(), nullptr, creation.dst.data()));
		cpis.clear();
	}

	// histogram pipes
	{
		addCpi(histogramPipeLayout, histogramPrepare_comp_spv_data);
		addCpi(histogramPipeLayout, histogramMax_comp_spv_data);
		addCpi(histogramPipeLayout, histogramPost_comp_spv_data);
		VkPipeline pipes[3];

		VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, VK_NULL_HANDLE,
			u32(cpis.size()), cpis.data(), nullptr, pipes));
		dstPipes.histogramPrepare = pipes[0];
		dstPipes.histogramMax = pipes[1];
		dstPipes.histogramPost = pipes[2];
		cpis.clear();
	}
}

} // namespace vil
