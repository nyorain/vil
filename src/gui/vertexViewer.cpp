#include <gui/vertexViewer.hpp>
#include <gui/commandHook.hpp>
#include <vk/format_utils.h>
#include <device.hpp>
#include <pipe.hpp>
#include <imgui/imgui.h>

#include <vertices.vert.spv.h>
#include <vertices.frag.spv.h>

namespace vil {

void VertexViewer::init(Device& dev, VkRenderPass rp) {
	dev_ = &dev;
	rp_ = rp;

	/*
	VkDescriptorSetLayoutBinding binding {};
	binding.binding = 0u;
	binding.descriptorCount = 1u;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo dslci {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 1u;
	dslci.pBindings = &binding;
	VK_CHECK(dev.dispatch.CreateDescriptorSetLayout(dev.handle, &dslci, nullptr, &dsLayout_));
	nameHandle(dev, this->dsLayout_, "VertexViewer:dsLayout");
	*/

	// pipeline layout
	// We just allocate the full push constant range that all implementations
	// must support.
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pcrs[0].offset = 0;
	pcrs[0].size = 128;

	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	// plci.setLayoutCount = 1;
	// plci.pSetLayouts = &dsLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr, &pipeLayout_));
	nameHandle(dev, this->pipeLayout_, "VertexViewer:pipeLayout");
}

VkPipeline VertexViewer::createPipe(VkFormat format, u32 stride,
		VkPrimitiveTopology topology) {
	auto& dev = *this->dev_;
	VkShaderModule vertModule, fragModule;

	VkShaderModuleCreateInfo vertShaderInfo {};
	vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertShaderInfo.codeSize = sizeof(vertices_vert_spv_data);
	vertShaderInfo.pCode = vertices_vert_spv_data;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &vertShaderInfo, NULL, &vertModule));

	VkShaderModuleCreateInfo fragShaderInfo {};
	fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fragShaderInfo.codeSize = sizeof(vertices_frag_spv_data);
	fragShaderInfo.pCode = vertices_frag_spv_data;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &fragShaderInfo, NULL, &fragModule));

	// store them for destruction later on
	std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertModule;
	stages[0].pName = "main";

	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragModule;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bindingDesc[1] = {};
	bindingDesc[0].stride = stride;
	bindingDesc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attribDesc[1] = {};
	attribDesc[0].location = 0;
	attribDesc[0].binding = bindingDesc[0].binding;
	attribDesc[0].format = format;
	attribDesc[0].offset = 0;

	VkPipelineVertexInputStateCreateInfo vertexInfo {};
	vertexInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInfo.vertexBindingDescriptionCount = 1;
	vertexInfo.pVertexBindingDescriptions = bindingDesc;
	vertexInfo.vertexAttributeDescriptionCount = 1;
	vertexInfo.pVertexAttributeDescriptions = attribDesc;

	VkPipelineInputAssemblyStateCreateInfo iaInfo {};
	iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	iaInfo.topology = topology;

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

	VkPipelineDepthStencilStateCreateInfo depthStencil {};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = true;
	depthStencil.depthWriteEnable = true;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.stencilTestEnable = false;

	VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynState {};
	dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo gpi[1] {};

	gpi[0].sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi[0].flags = 0;
	gpi[0].stageCount = stages.size();
	gpi[0].pStages = stages.data();
	gpi[0].pVertexInputState = &vertexInfo;
	gpi[0].pInputAssemblyState = &iaInfo;
	gpi[0].pViewportState = &viewportInfo;
	gpi[0].pRasterizationState = &rasterInfo;
	gpi[0].pMultisampleState = &msInfo;
	gpi[0].pDepthStencilState = &depthInfo;
	gpi[0].pColorBlendState = &blendInfo;
	gpi[0].pDepthStencilState = &depthStencil;
	gpi[0].pDynamicState = &dynState;
	gpi[0].layout = dev.renderData->pipeLayout;
	gpi[0].renderPass = rp_;
	gpi[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

	VkPipeline pipe;
	VK_CHECK(dev.dispatch.CreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, 1, gpi, nullptr, &pipe));

	auto& ourPipe = pipes_.emplace_back();
	ourPipe.pipe = pipe;
	ourPipe.topology = topology;
	ourPipe.format = format;
	ourPipe.stride = stride;

	return pipe;
}

void VertexViewer::imGuiDraw(VkCommandBuffer cb, const GraphicsPipeline& src,
		const CommandHookState& copies, std::optional<VkIndexType> indexType,
			u32 offset, u32 drawCount, u32 vertexOffset) {

	auto& dev = *this->dev_;

	// update input
	auto& io = ImGui::GetIO();

	auto mousePos = Vec2f{io.MousePos.x, io.MousePos.y};
	auto delta = lastMousPos_ - mousePos; // TODO: ignore the first time
	// TODO
	lastMousPos_ = mousePos;

	// try to find position by heuristics
	// TODO: cache this! But should likely not implemented here in first place.
	// TODO: implement a serious heuristic. Inspect the spv code,
	//   and try to find which input influences the Position output
	if(src.vertexAttribs.empty()) {
		dlg_info("Can't display vertices, no vertex attribs");
		return;
	}

	auto& attrib = src.vertexAttribs[0];
	auto& binding = src.vertexBindings[attrib.binding];

	dlg_assert(binding.binding < copies.vertexBufCopies.size());
	if(binding.binding > copies.vertexBufCopies.size()) {
		return;
	}

	auto& buf = copies.vertexBufCopies[binding.binding];

	// try to find matching pipeline
	VkPipeline foundPipe {};
	for(auto& pipe : pipes_) {
		if(pipe.format == attrib.format && pipe.stride == binding.stride) {
			foundPipe = pipe.pipe;
		}
	}

	if(!foundPipe) {
		foundPipe = createPipe(attrib.format, binding.stride, src.inputAssemblyState.topology);
	}

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, foundPipe);

	// TODO: push constants

	VkDeviceSize voffset = attrib.offset;
	dev.dispatch.CmdBindVertexBuffers(cb, 0, 1, &buf.buffer.buf, &voffset);

	if(indexType) {
		dlg_assert_or(copies.indexBufCopy.buffer.buf, return);
		dev.dispatch.CmdBindIndexBuffer(cb, copies.indexBufCopy.buffer.buf,
			0, *indexType);
		dev.dispatch.CmdDrawIndexed(cb, drawCount, 1, offset, vertexOffset, 0);
	} else {
		dev.dispatch.CmdDraw(cb, drawCount, 1, offset, 0);
	}
}

} // namespace vil
