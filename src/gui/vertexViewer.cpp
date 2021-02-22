#include <gui/vertexViewer.hpp>
#include <gui/commandHook.hpp>
#include <util/transform.hpp>
#include <device.hpp>
#include <commandDesc.hpp>
#include <commands.hpp>
#include <pipe.hpp>
#include <imgui/imgui.h>
#include <vk/format_utils.h>

#include <vertices.vert.spv.h>
#include <vertices.frag.spv.h>

namespace vil {

VertexViewer::~VertexViewer() {
	if(!dev_) {
		return;
	}

	for(auto& pipe : pipes_) {
		dev_->dispatch.DestroyPipeline(dev_->handle, pipe.pipe, nullptr);
	}

	dev_->dispatch.DestroyPipelineLayout(dev_->handle, pipeLayout_, nullptr);
	dev_->dispatch.DestroyShaderModule(dev_->handle, vertShader_, nullptr);
	dev_->dispatch.DestroyShaderModule(dev_->handle, fragShader_, nullptr);
}

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

	// load shaders
	VkShaderModuleCreateInfo vertShaderInfo {};
	vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertShaderInfo.codeSize = sizeof(vertices_vert_spv_data);
	vertShaderInfo.pCode = vertices_vert_spv_data;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &vertShaderInfo, NULL, &vertShader_));

	VkShaderModuleCreateInfo fragShaderInfo {};
	fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fragShaderInfo.codeSize = sizeof(vertices_frag_spv_data);
	fragShaderInfo.pCode = vertices_frag_spv_data;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &fragShaderInfo, NULL, &fragShader_));

}

VkPipeline VertexViewer::createPipe(VkFormat format, u32 stride,
		VkPrimitiveTopology topology) {
	auto& dev = *this->dev_;

	// store them for destruction later on
	std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertShader_;
	stages[0].pName = "main";

	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragShader_;
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
	// rasterInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterInfo.polygonMode = VK_POLYGON_MODE_LINE;
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
	gpi[0].stageCount = u32(stages.size());
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
		u32 offset, u32 drawCount, u32 vertexOffset,
		Vec2f canvasOffset, Vec2f canvasSize) {

	auto& dev = *this->dev_;

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
		if(pipe.format == attrib.format &&
				pipe.stride == binding.stride &&
				pipe.topology == src.inputAssemblyState.topology) {
			foundPipe = pipe.pipe;
		}
	}

	if(!foundPipe) {
		dlg_trace("creating new vertex viewer pipe");
		foundPipe = createPipe(attrib.format, binding.stride, src.inputAssemblyState.topology);
	}

	VkRect2D scissor {};
	scissor.offset.x = canvasOffset.x;
	scissor.offset.y = canvasOffset.y;
	scissor.extent.width = canvasSize.x;
	scissor.extent.height = canvasSize.y;
	dev.dispatch.CmdSetScissor(cb, 0, 1, &scissor);

	VkViewport viewport {};
	viewport.width = canvasSize.x;
	viewport.height = canvasSize.y;
	viewport.x = canvasOffset.x;
	viewport.y = canvasOffset.y;
	viewport.maxDepth = 1.f;
	dev.dispatch.CmdSetViewport(cb, 0, 1, &viewport);

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, foundPipe);
	dev.dispatch.CmdPushConstants(cb, pipeLayout_,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(viewProjMtx_), &viewProjMtx_);

	VkDeviceSize voffset = attrib.offset;
	dev.dispatch.CmdBindVertexBuffers(cb, 0, 1, &buf.buffer.buf, &voffset);

	// clear canvas color
	// TODO: make sure it's contained in the current render pass instance!
	VkClearAttachment clearAtt {};
	clearAtt.clearValue.color = {0.f, 0.f, 0.f, 1.f};
	clearAtt.colorAttachment = 0u;
	clearAtt.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkClearRect clearRect {};
	clearRect.rect.offset.x = canvasOffset.x;
	clearRect.rect.offset.y = canvasOffset.y;
	clearRect.rect.extent.width = canvasSize.x;
	clearRect.rect.extent.height = canvasSize.y;
	clearRect.layerCount = 1u;

	dev.dispatch.CmdClearAttachments(cb, 1u, &clearAtt, 1u, &clearRect);

	if(indexType) {
		dlg_assert_or(copies.indexBufCopy.buffer.buf, return);
		dev.dispatch.CmdBindIndexBuffer(cb, copies.indexBufCopy.buffer.buf,
			0, *indexType);
		dev.dispatch.CmdDrawIndexed(cb, drawCount, 1, offset, vertexOffset, 0);
	} else {
		dev.dispatch.CmdDraw(cb, drawCount, 1, offset, 0);
	}
}

void VertexViewer::updateInput(float dt) {
	// update input
	constexpr const auto pi = 3.14159265359;
	auto& io = ImGui::GetIO();

	if(ImGui::IsItemHovered()) {
		if(io.MouseDown[0]) {
			auto mousePos = Vec2f{io.MousePos.x, io.MousePos.y};
			if(rotating_) {
				auto delta = mousePos - lastMousPos_;
				(void) delta; // TODO: move cam

				constexpr const auto fac = 0.005f;
				constexpr const auto limitPitch = true;

				yaw_ = std::fmod(yaw_ - fac * delta.x, 2 * pi);
				pitch_ -= fac * delta.y;

				if(limitPitch) {
					auto pitchEps = 0.05;
					pitch_ = std::clamp<float>(pitch_, -pi / 2 + pitchEps, pi / 2 - pitchEps);
				}

				cam_.rot = Quaternion::yxz(yaw_, pitch_, 0.f);
				dlg_trace("cam yaw {}, pitch {}", yaw_, pitch_);
			}

			lastMousPos_ = mousePos;
		}

		rotating_ = io.MouseDown[0];
	}

	if(ImGui::IsItemFocused()) {
		auto right = apply(cam_.rot, Vec3f{1.f, 0.f, 0.f});
		auto up = Vec3f{0.f, 1.f, 0.f};
		auto fwd = apply(cam_.rot, Vec3f{0.f, 0.f, -1.f});
		Vec3f accel {};

		constexpr auto moveRight = swa_key_d;
		constexpr auto moveLeft = swa_key_a;
		constexpr auto moveFwd = swa_key_w;
		constexpr auto moveBwd = swa_key_s;
		constexpr auto moveUp = swa_key_q;
		constexpr auto moveDown = swa_key_e;

		if(io.KeysDown[moveRight]) {
			accel += right;
		}
		if(io.KeysDown[moveLeft]) {
			accel += -right;
		}
		if(io.KeysDown[moveFwd]) {
			accel += fwd;
		}
		if(io.KeysDown[moveBwd]) {
			accel += -fwd;
		}
		if(io.KeysDown[moveUp]) {
			accel += up;
		}
		if(io.KeysDown[moveDown]) {
			accel += -up;
		}

		auto fac = 100.f;
		if(io.KeyShift) {
			fac *= 5.f;
		}
		if(io.KeyCtrl) {
			fac *= 0.1f;
		}

		accel *= fac;
		cam_.pos += dt * accel;

		// TODO: not sure this is the right way to do this.
		// need to inform application that we have captured keyboard
		// input right now (when input comes from application)
		io.WantCaptureKeyboard = true;

		if(std::abs(accel.x) > 0.00001 || std::abs(accel.y) > 0.00001) {
			dlg_trace("cam pos: {}", cam_.pos);
		}
	}

	auto rect = ImGui::GetItemRectSize();
	auto aspect = rect.x / rect.y;

	constexpr auto fov = float(0.48 * pi);
	constexpr auto near = -0.01f;
	constexpr auto far = -10000.f;

	auto projMtx = perspective(fov, aspect, near, far);
	flipY(projMtx);

	auto viewMtx = viewMatrix(cam_);
	viewProjMtx_ = projMtx * viewMtx;
}

} // namespace vil
