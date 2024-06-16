#include <gui/vertexViewer.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <util/f16.hpp>
#include <util/util.hpp>
#include <util/spirv.hpp>
#include <util/fmt.hpp>
#include <device.hpp>
#include <shader.hpp>
#include <accelStruct.hpp>
#include <pipe.hpp>
#include <command/commands.hpp>
#include <nytl/transform.hpp>
#include <imgui/imgui.h>
#include <vk/format_utils.h>
#include <vkutil/enumString.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <vil_api.h>

#include <frustum.vert.spv.h>
#include <vertices.vert.spv.h>
#include <vertices.frag.spv.h>

namespace vil {

using nytl::read;

constexpr const auto pi = 3.14159265359;
constexpr auto fov = float(0.48 * pi);

// util
template<typename T>
std::string printFormat(u32 count, const Vec4d& val) {
	auto ret = std::string {};
	auto sep = "";
	for(auto i = 0u; i < count; ++i) {
		ret += dlg::format("{}{}", sep, T(val[i]));
		sep = ", ";
	}

	return ret;
}

// Need to convert the captures variable type info a vulkan format
// so that we can read and display it.
// TODO: should reuse buffmt for this.
VkFormat formatForType(const XfbCapture& capture) {
	auto compIDForWidth = [](u32 width) -> u32 {
		switch(width) {
			case 8: return 0u;
			case 16: return 1u;
			case 32: return 2u;
			case 64: return 3u;
			default: return u32(-1);
		}
	};

	// TODO
	dlg_assertm(capture.columns == 1u, "Matrix captures currently not supported");

	auto compID = compIDForWidth(capture.width);
	if(compID == u32(-1)) {
		dlg_error("Invalid width: {}", capture.width);
		return VK_FORMAT_UNDEFINED;
	}

	if(capture.type == XfbCapture::typeFloat) {
		VkFormat formats[4][4] = {
			{
				VK_FORMAT_UNDEFINED,
				VK_FORMAT_UNDEFINED,
				VK_FORMAT_UNDEFINED,
				VK_FORMAT_UNDEFINED,
			},
			{
				VK_FORMAT_R16_SFLOAT,
				VK_FORMAT_R16G16_SFLOAT,
				VK_FORMAT_R16G16B16_SFLOAT,
				VK_FORMAT_R16G16B16A16_SFLOAT,
			},
			{
				VK_FORMAT_R32_SFLOAT,
				VK_FORMAT_R32G32_SFLOAT,
				VK_FORMAT_R32G32B32_SFLOAT,
				VK_FORMAT_R32G32B32A32_SFLOAT,
			},
			{
				VK_FORMAT_R64_SFLOAT,
				VK_FORMAT_R64G64_SFLOAT,
				VK_FORMAT_R64G64B64_SFLOAT,
				VK_FORMAT_R64G64B64A64_SFLOAT,
			}
		};

		dlg_assert_or(capture.vecsize <= 4, return VK_FORMAT_UNDEFINED);
		dlg_assert_or(compID != 0u, return VK_FORMAT_UNDEFINED);
		return formats[compID][capture.vecsize - 1];
	} else if(capture.type == XfbCapture::typeUint) {
		VkFormat formats[4][4] = {
			{
				VK_FORMAT_R8_UINT,
				VK_FORMAT_R8G8_UINT,
				VK_FORMAT_R8G8B8_UINT,
				VK_FORMAT_R8G8B8A8_UINT,
			},
			{
				VK_FORMAT_R16_UINT,
				VK_FORMAT_R16G16_UINT,
				VK_FORMAT_R16G16B16_UINT,
				VK_FORMAT_R16G16B16A16_UINT,
			},
			{
				VK_FORMAT_R32_UINT,
				VK_FORMAT_R32G32_UINT,
				VK_FORMAT_R32G32B32_UINT,
				VK_FORMAT_R32G32B32A32_UINT,
			},
			{
				VK_FORMAT_R64_UINT,
				VK_FORMAT_R64G64_UINT,
				VK_FORMAT_R64G64B64_UINT,
				VK_FORMAT_R64G64B64A64_UINT,
			}
		};

		dlg_assert_or(capture.vecsize <= 4, return VK_FORMAT_UNDEFINED);
		return formats[compID][capture.vecsize - 1];
	} else if(capture.type == XfbCapture::typeInt) {
		VkFormat formats[4][4] = {
			{
				VK_FORMAT_R8_SINT,
				VK_FORMAT_R8G8_SINT,
				VK_FORMAT_R8G8B8_SINT,
				VK_FORMAT_R8G8B8A8_SINT,
			},
			{
				VK_FORMAT_R16_SINT,
				VK_FORMAT_R16G16_SINT,
				VK_FORMAT_R16G16B16_SINT,
				VK_FORMAT_R16G16B16A16_SINT,
			},
			{
				VK_FORMAT_R32_SINT,
				VK_FORMAT_R32G32_SINT,
				VK_FORMAT_R32G32B32_SINT,
				VK_FORMAT_R32G32B32A32_SINT,
			},
			{
				VK_FORMAT_R64_SINT,
				VK_FORMAT_R64G64_SINT,
				VK_FORMAT_R64G64B64_SINT,
				VK_FORMAT_R64G64B64A64_SINT,
			}
		};

		dlg_assert_or(capture.vecsize <= 4, return VK_FORMAT_UNDEFINED);
		return formats[compID][capture.vecsize - 1];
	}

	dlg_error("Invalid capture type {}", unsigned(capture.type));
	return VK_FORMAT_UNDEFINED;
}

std::string printFormat(VkFormat format, span<const std::byte> src) {
	u32 numChannels = FormatComponentCount(format);
	u32 componentSize = FormatElementSize(format) / numChannels;

	// TODO: not all formats covered
	//   support compresssed formats! See ioFormat in util.cpp
	// TODO: why do we care about componentSize??? shouldn't matter
	//  probably legacy, should be removed

	Vec4d val = read(format, src);

	if(FormatIsSFLOAT(format)) {
		switch(componentSize) {
			case 2: return printFormat<f16>(numChannels, val);
			case 4: return printFormat<float>(numChannels, val);
			case 8: return printFormat<double>(numChannels, val);
			default: break;
		}
	} else if(FormatIsUINT(format) || FormatIsUSCALED(format)) {
		switch(componentSize) {
			case 1: return printFormat<u16>(numChannels, val);
			case 2: return printFormat<u16>(numChannels, val);
			case 4: return printFormat<u32>(numChannels, val);
			case 8: return printFormat<u64>(numChannels, val);
			default: break;
		}
	} else if(FormatIsSINT(format) || FormatIsSSCALED(format)) {
		switch(componentSize) {
			case 1: return printFormat<i16>(numChannels, val);
			case 2: return printFormat<i16>(numChannels, val);
			case 4: return printFormat<i32>(numChannels, val);
			case 8: return printFormat<i64>(numChannels, val);
			default: break;
		}
	} else if(FormatIsUNORM(format)) {
		switch(componentSize) {
			case 1: return printFormat<u16> (numChannels, val);
			case 2: return printFormat<u16>(numChannels, val);
			default: break;
		}
	} else if(FormatIsSNORM(format)) {
		switch(componentSize) {
			case 1: return printFormat<i16> (numChannels, val);
			case 2: return printFormat<i16>(numChannels, val);
			default: break;
		}
	} else if(format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) {
		auto rgb = e5b9g9r9ToRgb(read<u32>(src));
		return dlg::format("{}", rgb[0], rgb[1], rgb[2]);
	}

	dlg_warn("Format {} not supported", vk::name(format));
	return "<Unsupported format>";
}

// NOTE: this could probably be improved
bool perspectiveHeuristic(ReadBuf data, u32 stride) {
	ZoneScoped;

	if(data.empty()) {
		dlg_warn("no data for orthogonal/perspective heuristic");
		return false;
	}

	auto vertBuf = data;
	auto first = read<Vec4f>(vertBuf);
	float firstW = first[3];
	bool nonConstW = false;
	bool nonOneW = false;

	for(auto i = 0u; i < std::min<u32>(20, u32(data.size() / stride)); ++i) {
		auto vertBuf = data.subspan(i * stride);
		auto vert = read<Vec4f>(vertBuf);

		if(std::abs(vert[3] - 1.f) > 0.001) {
			nonOneW = true;
		}

		if(std::abs(vert[3] - firstW) > 0.001) {
			nonConstW = true;
		}
	}

	if(nonConstW != nonOneW) {
		dlg_warn("conflicting data for orthogonal/perspective heuristic");
	}

	return nonOneW;
}

AABB3f bounds(VkFormat format, ReadBuf data, u32 stride, bool useW) {
	// dlg_assert(data.size() % stride == 0u);
	dlg_assert(data.size() >= stride);

	auto inf = std::numeric_limits<float>::infinity();
	auto min = Vec3f{inf, inf, inf};
	auto max = Vec3f{-inf, -inf, -inf};

	while(data.size() >= stride) {
		auto rem = data.subspan(stride);
		auto pos4 = read(format, data);
		auto pos3 = Vec3f(pos4);

		if(useW) {
			pos3.z = pos4[3];
		}

		min = vec::cw::min(min, pos3);
		max = vec::cw::max(max, pos3);
		data = rem;
	}

	// can probaby happen due to copied buffer truncation
	dlg_assertm(data.empty(), "Unexpected (unaligned) amount of vertex data");

	AABB3f ret;
	ret.pos = 0.5f * (min + max);
	ret.extent = 0.5f * (max - min);

	return ret;
}

AABB3f bounds(VkFormat vertFormat, ReadBuf vertData, u32 vertStride,
		VkIndexType indexType, ReadBuf indexData) {
	auto indSize = indexSize(indexType);
	dlg_assert(indSize > 0);
	dlg_assert(indexData.size() % indSize == 0u);

	auto inf = std::numeric_limits<float>::infinity();
	auto min = Vec3f{inf, inf, inf};
	auto max = Vec3f{-inf, -inf, -inf};

	while(indexData.size() >= indSize) {
		auto ind = readIndex(indexType, indexData);
		dlg_assert_or(ind * vertStride < vertData.size(), continue);

		auto vertBuf = vertData.subspan(ind * vertStride);
		auto pos = Vec3f(read(vertFormat, vertBuf));

		min = vec::cw::min(min, pos);
		max = vec::cw::max(max, pos);
	}

	AABB3f ret;
	ret.pos = 0.5f * (min + max);
	ret.extent = 0.5f * (max - min);

	return ret;
}

AABB3f bounds(span<const Vec4f> points, bool useW) {
	auto inf = std::numeric_limits<float>::infinity();
	auto min = Vec3f{inf, inf, inf};
	auto max = Vec3f{-inf, -inf, -inf};

	for(auto& point : points) {
		Vec3f pos3 = Vec3f(point);
		if(useW) {
			pos3.z = point[3];
		}

		min = vec::cw::min(min, pos3);
		max = vec::cw::max(max, pos3);
	}

	AABB3f ret;
	ret.pos = 0.5f * (min + max);
	ret.extent = 0.5f * (max - min);

	return ret;
}

AABB3f bounds(const AccelTriangles::Geometry& tris) {
	auto inf = std::numeric_limits<float>::infinity();
	auto min = Vec3f{inf, inf, inf};
	auto max = Vec3f{-inf, -inf, -inf};

	for(auto& tri : tris.triangles) {
		min = vec::cw::min(min, Vec3f(tri.a));
		max = vec::cw::max(max, Vec3f(tri.a));

		min = vec::cw::min(min, Vec3f(tri.b));
		max = vec::cw::max(max, Vec3f(tri.b));

		min = vec::cw::min(min, Vec3f(tri.c));
		max = vec::cw::max(max, Vec3f(tri.c));
	}

	AABB3f ret;
	ret.pos = 0.5f * (min + max);
	ret.extent = 0.5f * (max - min);

	return ret;
}

AABB3f bounds(const AccelTriangles& tris) {
	auto inf = std::numeric_limits<float>::infinity();
	auto min = Vec3f{inf, inf, inf};
	auto max = Vec3f{-inf, -inf, -inf};

	for(auto& geom : tris.geometries) {
		auto b = bounds(geom);
		min = vec::cw::min(min, b.pos - b.extent);
		max = vec::cw::max(max, b.pos + b.extent);
	}

	AABB3f ret;
	ret.pos = 0.5f * (min + max);
	ret.extent = 0.5f * (max - min);

	return ret;
}

// VertexViewer
VertexViewer::~VertexViewer() {
	if(!gui_) {
		return;
	}

	auto& dev = gui_->dev();
	for(auto& pipe : pipes_) {
		dev.dispatch.DestroyPipeline(dev.handle, pipe.pipe, nullptr);
	}

	dev.dispatch.DestroyPipeline(dev.handle, frustumPipe_, nullptr);
	dev.dispatch.DestroyPipelineLayout(dev.handle, pipeLayout_, nullptr);
	dev.dispatch.DestroyShaderModule(dev.handle, vertShader_, nullptr);
	dev.dispatch.DestroyShaderModule(dev.handle, fragShader_, nullptr);
}

void VertexViewer::init(Gui& gui) {
	gui_ = &gui;

	auto& dev = gui_->dev();

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

	if(dev.nonSolidFill) {
		createFrustumPipe();
	}
}

void VertexViewer::createFrustumPipe() {
	auto& dev = gui_->dev();
	dlg_assert(dev.nonSolidFill);

	// load shaders
	VkShaderModule vertShader;

	VkShaderModuleCreateInfo vertShaderInfo {};
	vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertShaderInfo.codeSize = sizeof(frustum_vert_spv_data);
	vertShaderInfo.pCode = frustum_vert_spv_data;
	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &vertShaderInfo, NULL, &vertShader));

	std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertShader;
	stages[0].pName = "main";

	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragShader_;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertexInfo {};
	vertexInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo iaInfo {};
	iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

	VkPipelineViewportStateCreateInfo viewportInfo {};
	viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportInfo.viewportCount = 1;
	viewportInfo.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterInfo {};
	rasterInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
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
	gpi[0].layout = gui_->imguiPipeLayout().vkHandle();
	gpi[0].renderPass = gui_->rp();
	gpi[0].flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;

	VK_CHECK(dev.dispatch.CreateGraphicsPipelines(dev.handle,
		VK_NULL_HANDLE, 1, gpi, nullptr, &frustumPipe_));

	dev.dispatch.DestroyShaderModule(dev.handle, vertShader, nullptr);
}

VkPipeline VertexViewer::createPipe(VkFormat format, u32 stride,
		VkPrimitiveTopology topology) {
	auto& dev = gui_->dev();

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
	bindingDesc[0].binding = 0u;

	VkVertexInputAttributeDescription attribDesc[1] = {};
	attribDesc[0].location = 0;
	attribDesc[0].binding = 0u;
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
	// TODO: Allow to toggle? Or draw both?
	rasterInfo.polygonMode = dev.nonSolidFill ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
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
	gpi[0].layout = gui_->imguiPipeLayout().vkHandle();
	gpi[0].renderPass = gui_->rp();
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

void VertexViewer::imGuiDraw(const DrawData& data) {
	ZoneScoped;

	auto& dev = gui_->dev();
	auto cb = data.cb;

	// try to find position by heuristics
	// TODO: cache this! But should likely not be implemented here in first place.
	// TODO: implement a serious heuristic. Inspect the spv code,
	//   and try to find which input influences the Position output
	auto& vinput = data.vertexInput;
	if(vinput.attribs.empty()) {
		dlg_info("Can't display vertices, no vertex attribs");
		return;
	}

	dlg_assert(!vinput.bindings.empty());

	auto& attrib = vinput.attribs[0];
	auto& binding = vinput.bindings[attrib.binding];

	dlg_assert_or(binding.binding < data.vertexBuffers.size(), return);
	auto& vbuf = data.vertexBuffers[binding.binding];
	auto voffset = VkDeviceSize(vbuf.offset + attrib.offset);

	// try to find matching pipeline
	VkPipeline foundPipe {};
	for(auto& pipe : pipes_) {
		if(pipe.format == attrib.format &&
				pipe.stride == binding.stride &&
				pipe.topology == data.topology) {
			foundPipe = pipe.pipe;
		}
	}

	if(!foundPipe) {
		foundPipe = createPipe(attrib.format, binding.stride, data.topology);
	}

	auto displaySize = ImGui::GetIO().DisplaySize;

	VkRect2D scissor {};
	scissor.offset.x = std::max<int>(data.canvasOffset.x, 0);
	scissor.offset.y = std::max<int>(data.canvasOffset.y, 0);
	scissor.extent.width = std::min<int>(
		data.canvasSize.x + data.canvasOffset.x - scissor.offset.x,
		displaySize.x - data.canvasOffset.x);
	scissor.extent.height = std::min<int>(
		data.canvasSize.y + data.canvasOffset.y - scissor.offset.y,
		displaySize.y - data.canvasOffset.y);
	dev.dispatch.CmdSetScissor(cb, 0, 1, &scissor);

	VkViewport viewport {};
	viewport.width = scissor.extent.width;
	viewport.height = scissor.extent.height;
	viewport.x = scissor.offset.x;
	viewport.y = scissor.offset.y;
	viewport.maxDepth = 1.f;
	dev.dispatch.CmdSetViewport(cb, 0, 1, &viewport);

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, foundPipe);

	struct {
		Mat4f matrix;
		u32 useW;
		float scale;
	} pcData = {
		viewProjMtx_ * data.mat,
		data.useW,
		data.scale,
	};

	dev.dispatch.CmdPushConstants(cb, pipeLayout_,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(pcData), &pcData);

	dev.dispatch.CmdBindVertexBuffers(cb, 0, 1, &vbuf.buffer, &voffset);

	// clear canvas color
	VkClearAttachment clearAtt {};
	clearAtt.clearValue.color = {0.f, 0.f, 0.f, 1.f};
	clearAtt.colorAttachment = 0u;
	clearAtt.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkClearRect clearRect {};
	clearRect.rect = scissor;
	clearRect.layerCount = 1u;

	if(data.clear) {
		dev.dispatch.CmdClearAttachments(cb, 1u, &clearAtt, 1u, &clearRect);
	}

	if(data.params.indexType) {
		dlg_assert_or(data.indexBuffer.buffer, return);
		dlg_assert_or(data.indexBuffer.size, return);

		dev.dispatch.CmdBindIndexBuffer(cb, data.indexBuffer.buffer,
			0, *data.params.indexType);
		dev.dispatch.CmdDrawIndexed(cb, data.params.drawCount, 1,
			data.params.offset, data.params.vertexOffset, data.params.instanceID);
	} else {
		dev.dispatch.CmdDraw(cb, data.params.drawCount, 1,
			data.params.offset, data.params.instanceID);
	}

	if(data.drawFrustum) {
		struct {
			Mat4f matrix;
			float near;
			float far;
			u32 useW;
		} pcData = {
			viewProjMtx_,
			// TODO: we could calculate them from perspective z, w values
			0.f,
			data.useW ? 100000.f : 1.f,
			data.useW,
		};

		dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, frustumPipe_);
		dev.dispatch.CmdPushConstants(cb, pipeLayout_,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0, sizeof(pcData), &pcData);
		dev.dispatch.CmdDraw(cb, 24, 1, 0, 0);
	}
}

void VertexViewer::updateInput(float dt) {
	// update input
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

		constexpr auto moveRight = ImGuiKey_D;
		constexpr auto moveLeft = ImGuiKey_A;
		constexpr auto moveFwd = ImGuiKey_W;
		constexpr auto moveBwd = ImGuiKey_S;
		constexpr auto moveUp = ImGuiKey_Q;
		constexpr auto moveDown = ImGuiKey_E;

		if(ImGui::IsKeyDown(moveRight)) {
			accel += right;
		}
		if(ImGui::IsKeyDown(moveLeft)) {
			accel += -right;
		}
		if(ImGui::IsKeyDown(moveFwd)) {
			accel += fwd;
		}
		if(ImGui::IsKeyDown(moveBwd)) {
			accel += -fwd;
		}
		if(ImGui::IsKeyDown(moveUp)) {
			accel += up;
		}
		if(ImGui::IsKeyDown(moveDown)) {
			accel += -up;
		}

		auto fac = speed_;
		if(io.KeyShift) {
			fac *= 5.f;
		}
		if(io.KeyCtrl) {
			fac *= 0.1f;
		}

		accel *= fac;
		cam_.pos += dt * accel;

		// NOTE: not sure this is the right way to do this.
		// need to inform application that we have captured keyboard
		// input right now (when input comes from application)
		io.WantCaptureKeyboard = true;
	}

	auto rect = ImGui::GetItemRectSize();
	auto aspect = rect.x / rect.y;

	auto projMtx = perspective(fov, aspect, near_, far_);
	flipY(projMtx);

	auto viewMtx = viewMatrix(cam_);
	viewProjMtx_ = projMtx * viewMtx;
}

void VertexViewer::displayInput(Draw& draw, const DrawCmdBase& cmd,
		const CommandHookState& state, float dt) {
	ZoneScoped;

	dlg_assert_or(cmd.state->pipe, return);
	if(state.vertexBufCopies.size() < cmd.state->pipe->vertexBindings.size()) {
		ImGui::Text("Error: not enough vertex buffers bound. See log output");
		return;
	}

	auto& pipe = *cmd.state->pipe;

	const PipelineShaderStage* vertStage = nullptr;
	for(auto& stage : pipe.stages) {
		if(stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
			vertStage = &stage;
			break;
		}
	}

	if(!vertStage) {
		// TODO: yeah this can happen with mesh shaders now
		ImGui::Text("Error: Grahpics Pipeline has no vertex stage :o");
		return;
	}

	// match bindings to input variables into
	// (pipe.vertexAttrib, vertStage->input_variables) id pairs
	std::vector<std::pair<u32, u32>> attribs;
	auto& compiled = specializeSpirv(*vertStage);
	auto resources = compiled.get_shader_resources();

	for(auto a = 0u; a < pipe.vertexAttribs.size(); ++a) {
		auto& attrib = pipe.vertexAttribs[a];
		auto found = false;
		for(auto i = 0u; i < resources.stage_inputs.size(); ++i) {
			auto& iv = resources.stage_inputs[i];
			if(!compiled.has_decoration(iv.id, spv::DecorationLocation)) {
				dlg_warn("vertex shader input without decoration location?");
				continue;
			}
			auto loc = compiled.get_decoration(iv.id, spv::DecorationLocation);
			if(loc == attrib.location) {
				dlg_assertm(!found, "Multiple input vars for the same attrib?!");
				attribs.push_back({a, i});
				found = true;

				// only in debug mode we want to make sure there aren't multiple
				// input vars
#ifndef VIL_DEBUG
				break;
#endif // VIL_DEBUG
			}
		}
	}

	// first, get draw params
	DrawParams params;

	auto displayCmdSlider = [&](std::optional<VkIndexType> indices, u32 stride, u32 offset = 0u){
		dlg_assert(gui_->dev().commandHook->ops().copyIndirectCmd);
		dlg_assert(state.indirectCopy.size);

		auto count = state.indirectCommandCount;
		if(count == 0u) {
			imGuiText("No commands (drawCount = 0)");
			selectedID_ = -1;
			return;
		} else if(count == 1u) {
			selectedID_ = 0u;
		} else {
			auto lbl = dlg::format("Commands: {}", count);
			optSliderRange(lbl.c_str(), selectedID_, count);
		}

		auto& ic = state.indirectCopy;
		auto span = ic.data().subspan(offset);
		if(indices) {
			auto sub = span.subspan(selectedID_ * stride);
			auto ecmd = read<VkDrawIndexedIndirectCommand>(sub);
			params.offset = ecmd.firstIndex;
			params.drawCount = ecmd.indexCount;
			params.vertexOffset = ecmd.vertexOffset;
			params.indexType = indices;
			params.instanceID = ecmd.firstInstance;
		} else {
			auto sub = span.subspan(selectedID_ * stride);
			auto ecmd = read<VkDrawIndirectCommand>(sub);
			params.offset = ecmd.firstVertex;
			params.drawCount = ecmd.vertexCount;
			params.instanceID = ecmd.firstInstance;
		}
	};

	if(auto* dcmd = commandCast<const DrawCmd*>(&cmd); dcmd) {
		params.offset = dcmd->firstVertex;
		params.drawCount = dcmd->vertexCount;
		params.instanceID = dcmd->firstInstance;
	} else if(auto* dcmd = commandCast<const DrawIndexedCmd*>(&cmd); dcmd) {
		params.offset = dcmd->firstIndex;
		params.vertexOffset = dcmd->vertexOffset;
		params.drawCount = dcmd->indexCount;
		params.indexType = dcmd->state->indices.type;
		params.instanceID = dcmd->firstInstance;
	} else if(auto* dcmd = commandCast<const DrawIndirectCmd*>(&cmd); dcmd) {
		auto i = dcmd->indexed ? std::optional(dcmd->state->indices.type) : std::nullopt;
		displayCmdSlider(i, dcmd->stride);
	} else if(auto* dcmd = commandCast<const DrawIndirectCountCmd*>(&cmd); dcmd) {
		auto i = dcmd->indexed ? std::optional(dcmd->state->indices.type) : std::nullopt;
		displayCmdSlider(i, dcmd->stride, 4u); // skip u32 count
	} else {
		imGuiText("Vertex viewer unimplemented for command type");
		return;
	}

	// TODO sort attribs by input location?

	auto colCount = attribs.size();
	if(params.indexType) {
		++colCount;
	}

	// clamp drawCount to captured size
	if(params.indexType) {
		auto maxCount = state.indexBufCopy.size / indexSize(*params.indexType);
		params.drawCount = std::min<u32>(params.drawCount, maxCount);

		// TODO: we also have to clamp to size of captured vertex buffers
		// But for that we'd have to iterate over all the indices, too
		// expensive here on cpu.
		//   -> vertex buffer capture rework
	} else {
		for(auto& [pipeAttribID, shaderVarID] : attribs) {
			auto& attrib = pipe.vertexAttribs[pipeAttribID];
			auto& binding = pipe.vertexBindings[attrib.binding];
			auto& buf = state.vertexBufCopies[binding.binding];

			auto maxCount = buf.size / binding.stride; // floor
			if(binding.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
				// no-op atm, limit instances drawn
			} else if(binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX) {
				params.drawCount = std::min<u32>(params.drawCount, maxCount - params.vertexOffset);
			}
		}
	}

	auto flags = ImGuiTableFlags_BordersInner | ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
	if(ImGui::BeginChild("vertexTable", {0.f, 200.f})) {
		if(colCount == 0u) {
			ImGui::Text("No Vertex input");
		} else if(ImGui::BeginTable("Vertices", colCount, flags)) {
			ImGui::TableNextRow();

			if(params.indexType) {
				// ImGui::TableSetupColumn("IDX");
				ImGui::TableNextColumn();
				imGuiText("IDX");
			}

			for(auto& [pipeAttribID, shaderVarID] : attribs) {
				auto& iv = resources.stage_inputs[shaderVarID];
				auto name = iv.name.empty() ? "<unnamed>" : iv.name.c_str();
				// ImGui::TableSetupColumn(name);

				ImGui::TableNextColumn();
				imGuiText(name);

				if(ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();

					auto& attrib = pipe.vertexAttribs[pipeAttribID];
					auto& binding = pipe.vertexBindings[attrib.binding];
					auto& buf = state.vertexBufCopies[binding.binding];

					imGuiText("pipeline attribute ID: {}", pipeAttribID);
					imGuiText("attrib format: {}", vk::name(attrib.format));
					imGuiText("shader input location: {}", attrib.location);
					imGuiText("attribute offset: {}", attrib.offset);
					imGuiText("vertex buffer ID: {}", binding.binding);
					imGuiText("captured buffer size: {}", buf.size);
					imGuiText("buffer stride: {}", binding.stride);
					imGuiText("buffer input rate: {}", vk::name(binding.inputRate));

					ImGui::EndTooltip();
				}
			}

			// ImGui::TableHeadersRow();
			ImGui::TableNextRow();

			for(auto i = 0u; i < std::min(100u, params.drawCount); ++i) {
				bool captured = true;
				auto vertexID = i;
				auto iniID = params.instanceID;
				if(params.indexType) {
					ImGui::TableNextColumn();

					auto is = indexSize(*params.indexType);
					auto ib = state.indexBufCopy.data();
					auto off = (params.offset + i) * is;
					if(off + is > ib.size()) {
						captured = false;
						imGuiText("N/A");
					} else {
						ib = ib.subspan(off, is);
						vertexID = readIndex(*params.indexType, ib) + params.vertexOffset;
						imGuiText("{}", vertexID);
					}
				} else {
					vertexID += params.offset;
					// XXX this isn't needed, right? not sure.
					// iniID += params.offset;
				}

				for(auto& [aID, _] : attribs) {
					auto& attrib = pipe.vertexAttribs[aID];
					ImGui::TableNextColumn();

					auto& binding = pipe.vertexBindings[attrib.binding];
					auto& buf = state.vertexBufCopies[binding.binding];

					auto off = (binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX) ?
						vertexID * binding.stride :
						iniID * binding.stride;
					off += attrib.offset;

					// TODO: compressed support?
					auto size = FormatElementSize(attrib.format);
					if(off + size > buf.data().size()) {
						captured = false;
					}

					if(!captured) {
						imGuiText("N/A");
						continue;
					}

					auto* ptr = buf.data().data() + off;
					auto str = printFormat(attrib.format, {ptr, size});

					imGuiText("{}", str);
				}

				ImGui::TableNextRow();
			}

			ImGui::EndTable();
		}
	}

	ImGui::EndChild();

	// 2: viewer
	if(ImGui::Button("Recenter")) {
		auto& attrib = pipe.vertexAttribs[0];
		auto& binding = pipe.vertexBindings[attrib.binding];

		AABB3f vertBounds;
		auto vertData = state.vertexBufCopies[binding.binding].data();
		vertData = vertData.subspan(attrib.offset);
		if(params.indexType) {
			vertData = vertData.subspan(params.vertexOffset * binding.stride);
			auto indData = state.indexBufCopy.data();
			auto offset = indexSize(*params.indexType) * params.offset;
			auto size = indexSize(*params.indexType) * params.drawCount;
			indData = indData.subspan(offset, size);
			vertBounds = bounds(attrib.format, vertData, binding.stride,
				*params.indexType, indData);
		} else {
			auto offset = params.offset * binding.stride;
			auto size = params.drawCount * binding.stride;
			vertData = vertData.subspan(offset, size);
			vertBounds = bounds(attrib.format, vertData, binding.stride, false);
		}

		centerCamOnBounds(vertBounds);
	}

	if(ImGui::BeginChild("vertexViewer")) {
		auto avail = ImGui::GetContentRegionAvail();
		auto pos = ImGui::GetCursorScreenPos();

		auto attribCount = pipe.vertexInputState.vertexAttributeDescriptionCount;
		auto bindingCount = pipe.vertexInputState.vertexBindingDescriptionCount;

		auto& vinput = drawData_.vertexInput;
		vinput.attribs.resize(attribCount);
		vinput.bindings.resize(bindingCount);

		std::copy(pipe.vertexInputState.pVertexAttributeDescriptions,
			pipe.vertexInputState.pVertexAttributeDescriptions + attribCount,
			vinput.attribs.begin());
		std::copy(pipe.vertexInputState.pVertexBindingDescriptions,
			pipe.vertexInputState.pVertexBindingDescriptions + bindingCount,
			vinput.bindings.begin());

		if(pipe.dynamicState.count(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT)) {
			for(auto i = 0u; i < bindingCount; ++i) {
				auto& buf = cmd.state->vertices[i];
				if(buf.stride) {
					vinput.bindings[i].stride = buf.stride;
				}
			}
		}

		drawData_.cb = draw.cb;
		drawData_.params = params;
		drawData_.canvasOffset = {pos.x, pos.y};
		drawData_.canvasSize = {avail.x, avail.y};
		drawData_.topology = pipe.inputAssemblyState.topology;
		drawData_.vertexBuffers = {};
		drawData_.scale = 1.f;
		drawData_.useW = false;
		drawData_.drawFrustum = false;

		for(auto& buf : state.vertexBufCopies) {
			drawData_.vertexBuffers.push_back({buf.buf, 0u, buf.size});
		}

		if(params.indexType) {
			drawData_.indexBuffer = {state.indexBufCopy.buf, 0u, state.indexBufCopy.size};
		}

		auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
			auto* self = static_cast<VertexViewer*>(cmd->UserCallbackData);
			self->imGuiDraw(self->drawData_);
		};

		ImGui::GetWindowDrawList()->AddCallback(cb, this);
		ImGui::InvisibleButton("Canvas", avail);
		updateInput(dt);

		dlg_assert(!draw.usedHookState);
		draw.usedHookState.reset(const_cast<CommandHookState*>(&state));
	}

	ImGui::EndChild();
}

u32 topologyOutputCount(VkPrimitiveTopology topo, i32 in) {
	switch(topo) {
		case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
			return u32(in);

		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
			return u32(2 * std::max(0, in - 1));

		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
			return u32(in / 2);
		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
			return u32(2 * std::max(0, in - 3));

		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
			return u32(3 * std::max(0, in - 2));

		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
			return u32(in / 2);
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
		case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
			dlg_warn("Not implemeneted"); // ugh, no idea
			return 0u;

		default:
			dlg_error("Invalid topology {}", u32(topo));
			return 0u;
	}
}

const char* name(spv11::BuiltIn builtin) {
	switch(builtin) {
		case spv11::BuiltIn::Position: return "Position";
		case spv11::BuiltIn::PointSize: return "PointSize";
		case spv11::BuiltIn::ClipDistance: return "ClipDistance";
		case spv11::BuiltIn::CullDistance: return "CullDistance";
		default: return "unknown builtin";
	}
}

void VertexViewer::displayOutput(Draw& draw, const DrawCmdBase& cmd,
		const CommandHookState& state, float dt) {
	ZoneScoped;
	dlg_assert_or(cmd.state->pipe, return);
	auto& pipe = *cmd.state->pipe;

	if(!pipe.xfbPatch) {
		imGuiText("Error: couldn't inject transform feedback code to shader");
		return;
	} else if(!state.transformFeedback.size) {
		ImGui::Text("Error: no transform feedback. See log output");
		return;
	}

	auto& xfbPatch = *pipe.xfbPatch;

	u32 vertexOffset {};
	u32 vertexCount {};
	auto displayCmdSlider = [&](bool indexed, u32 stride, u32 offset = 0u){
		dlg_assert(gui_->dev().commandHook->ops().copyIndirectCmd);
		dlg_assert(state.indirectCopy.size);

		auto count = state.indirectCommandCount;
		if(count == 0u) {
			imGuiText("No commands (drawCount = 0)");
			selectedID_ = -1;
			return;
		} else if(count == 1u) {
			selectedID_ = 0u;
		} else {
			auto lbl = dlg::format("Commands: {}", count);
			optSliderRange(lbl.c_str(), selectedID_, count);
		}

		auto& ic = state.indirectCopy;
		auto span = ic.data().subspan(offset);
		if(indexed) {
			for(auto i = 0u; i < selectedID_; ++i) {
				auto sub = span.subspan(i * stride);
				auto ecmd = read<VkDrawIndexedIndirectCommand>(sub);
				vertexOffset += ecmd.indexCount * ecmd.instanceCount;
			}

			auto sub = span.subspan(selectedID_ * stride);
			auto ecmd = read<VkDrawIndexedIndirectCommand>(sub);
			vertexCount = ecmd.indexCount * ecmd.instanceCount;
		} else {
			for(auto i = 0u; i < selectedID_; ++i) {
				auto sub = span.subspan(i * stride);
				auto ecmd = read<VkDrawIndirectCommand>(sub);
				vertexOffset += ecmd.vertexCount * ecmd.instanceCount;
			}

			auto sub = span.subspan(selectedID_ * stride);
			auto ecmd = read<VkDrawIndirectCommand>(sub);
			vertexCount = ecmd.vertexCount * ecmd.instanceCount;
		}
	};

	if(auto* dcmd = commandCast<const DrawCmd*>(&cmd); dcmd) {
		vertexCount = dcmd->vertexCount * dcmd->instanceCount;
	} else if(auto* dcmd = commandCast<const DrawIndexedCmd*>(&cmd); dcmd) {
		vertexCount = dcmd->indexCount * dcmd->instanceCount;
	} else if(auto* dcmd = commandCast<const DrawIndirectCmd*>(&cmd); dcmd) {
		displayCmdSlider(dcmd->indexed, dcmd->stride);
		if(selectedID_ == u32(-1)) {
			return;
		}
	} else if(auto* dcmd = commandCast<const DrawIndirectCountCmd*>(&cmd); dcmd) {
		displayCmdSlider(dcmd->indexed, dcmd->stride, 4u); // skip u32 count in the beginning
		if(selectedID_ == u32(-1)) {
			return;
		}
	} else {
		imGuiText("Vertex viewer unimplemented for command type");
		return;
	}

	vertexCount = topologyOutputCount(pipe.inputAssemblyState.topology, vertexCount);
	vertexOffset = topologyOutputCount(pipe.inputAssemblyState.topology, vertexOffset);

	auto capturedCount = state.transformFeedback.size / xfbPatch.stride;
	if(vertexOffset + vertexCount > capturedCount) {
		dlg_warn("xfb data truncated; Would need huge buffers");

		if(vertexOffset >= capturedCount) {
			imGuiText("Nothing to display; Not enough data captured");
			return;
		}

		vertexCount = capturedCount - vertexOffset;
	}

	// 1: table
	auto flags = ImGuiTableFlags_BordersInner | ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
	if(ImGui::BeginChild("vertexTable", {0.f, 200.f})) {
		if(ImGui::BeginTable("Vertices", xfbPatch.captures.size(), flags)) {
			ZoneScopedN("Table");

			// header
			for(auto& capture : xfbPatch.captures) {
				auto name = capture.name.c_str();
				if(capture.builtin) {
					name = vil::name(spv11::BuiltIn(*capture.builtin));
				}

				if(!capture.array.empty()) {
					// TODO: print all array elements. Make sure to intitialize
					// column count correctly in BeginTable!
					auto sname = std::string(name) + "[0]";
					ImGui::TableSetupColumn(sname.c_str());
				} else {
					ImGui::TableSetupColumn(name);
				}
			}
			ImGui::TableHeadersRow();
			ImGui::TableNextRow();

			// data
			auto xfbData = state.transformFeedback.data();
			xfbData = xfbData.subspan(vertexOffset * xfbPatch.stride);

			// TODO: show pages
			// TODO: make rows selectable
			for(auto i = 0u; i < std::min(u32(100u), vertexCount); ++i) {
				auto buf = xfbData.subspan(i * xfbPatch.stride, xfbPatch.stride);

				for(auto& capture : xfbPatch.captures) {
					ImGui::TableNextColumn();

					auto cbuf = buf.subspan(capture.offset);
					auto format = formatForType(capture);
					if(format == VK_FORMAT_UNDEFINED) {
						imGuiText("<error>");
					} else {
						imGuiText("{}", printFormat(format, cbuf));
					}
				}

				ImGui::TableNextRow();
			}

			ImGui::EndTable();
		}
	}

	ImGui::EndChild();

	// 2: viewer
	// NOTE: strictly speaking the reinterepret_cast is UB but it's
	// all just trivial types so who cares

	auto bspan = state.transformFeedback.data();

	XfbCapture* posCapture = nullptr;
	for(auto& patch : xfbPatch.captures) {
		if(patch.builtin && *patch.builtin == u32(spv11::BuiltIn::Position)) {
			posCapture = &patch;
			break;
		}
	}

	if(!posCapture) {
		// This might happen e.g. for geometry/tesselation pipes.
		// TODO: just allow to select an attribute to be used as position in gui
		imGuiText("No BuiltIn Position");
		return;
	}

	dlg_assert(posCapture->type == XfbCapture::typeFloat);
	dlg_assert(posCapture->vecsize == 4u);
	dlg_assert(posCapture->width == 32);
	dlg_assert(posCapture->columns == 1u);
	dlg_assert(posCapture->array.empty());

	bspan = bspan.subspan(vertexOffset * xfbPatch.stride + posCapture->offset,
		vertexCount * xfbPatch.stride);

	// TODO: don't evaluate this every frame, just in the beginning
	// and when the Recenter button is pressed.
	const bool useW = perspectiveHeuristic(bspan, xfbPatch.stride);

	if(ImGui::Button("Recenter")) {
		auto vertBounds = bounds(VK_FORMAT_R32G32B32A32_SFLOAT, bspan, xfbPatch.stride, useW);
		centerCamOnBounds(vertBounds);
	}

	if(ImGui::BeginChild("vertexViewer")) {
		auto avail = ImGui::GetContentRegionAvail();
		auto pos = ImGui::GetCursorScreenPos();

		// we statically know the single binding and attribute
		auto& vinput = drawData_.vertexInput;
		vinput.bindings.resize(1);
		vinput.attribs.resize(1);

		vinput.bindings[0] = {
			0u, xfbPatch.stride, VK_VERTEX_INPUT_RATE_VERTEX,
		};

		vinput.attribs[0] = {
			0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, posCapture->offset,
		};

		auto& xfbBuf = state.transformFeedback;
		drawData_.cb = draw.cb;
		drawData_.params = {};
		drawData_.params.drawCount = vertexCount;
		drawData_.params.offset = vertexOffset;
		drawData_.indexBuffer = {};
		drawData_.vertexBuffers = {{{xfbBuf.buf, 0u, xfbBuf.size}}};
		drawData_.canvasOffset = {pos.x, pos.y};
		drawData_.canvasSize = {avail.x, avail.y};
		drawData_.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		drawData_.useW = useW;
		drawData_.scale = 1.f;
		drawData_.drawFrustum = gui_->dev().nonSolidFill;
		drawData_.clear = true;
		drawData_.mat = nytl::identity<4, float>();

		auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
			auto* self = static_cast<VertexViewer*>(cmd->UserCallbackData);
			self->imGuiDraw(self->drawData_);
		};

		ImGui::GetWindowDrawList()->AddCallback(cb, this);
		ImGui::InvisibleButton("Canvas", avail);
		updateInput(dt);

		// we read from the buffer that is potentially written again
		// by the hook so we need barriers.
		dlg_assert(!draw.usedHookState);
		draw.usedHookState.reset(const_cast<CommandHookState*>(&state));
	}

	ImGui::EndChild();
}

void VertexViewer::centerCamOnBounds(const AABB3f& bounds) {
	auto mxy = std::max(bounds.extent.y, bounds.extent.x);
	auto l = mxy / std::tan(0.5f * fov);

	// cam looks along -z by default
	cam_.pos = bounds.pos + Vec3f{0.f, 0.f, bounds.extent.z} + l * Vec3f{0.f, 0.f, 1.f};
	cam_.rot = Quaternion {};

	yaw_ = {};
	pitch_ = {};

	auto sum = bounds.extent.x + bounds.extent.y + bounds.extent.z;
	speed_ = sum;
	near_ = -0.001 * sum;
	far_ = -100 * sum;
}

void VertexViewer::displayTriangles(Draw& draw, const OwnBuffer& buf,
		const AccelTriangles& tris, float dt) {
	ZoneScoped;

	if(ImGui::Button("Recenter")) {
		AABB3f vertBounds = bounds(tris);
		centerCamOnBounds(vertBounds);
	}

	if(ImGui::BeginChild("vertexViewer")) {
		auto avail = ImGui::GetContentRegionAvail();
		auto pos = ImGui::GetCursorScreenPos();

		// we statically know the single binding and attribute
		auto& vinput = drawData_.vertexInput;
		vinput.bindings.resize(1);
		vinput.attribs.resize(1);

		vinput.bindings[0] = {
			0u, sizeof(Vec4f), VK_VERTEX_INPUT_RATE_VERTEX,
		};

		vinput.attribs[0] = {
			0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u,
		};

		// TODO: we should multiple draw calls instead of just batching
		// everything together. E.g. to visualize different geometry flags
		// or to color them differently.
		auto drawCount = 0u;
		for(auto& geom : tris.geometries) {
			drawCount += geom.triangles.size() * 3;
		}

		drawData_.cb = draw.cb;
		drawData_.params = {};
		drawData_.params.drawCount = drawCount;
		drawData_.indexBuffer = {};
		drawData_.vertexBuffers = {{{buf.buf, 0u, buf.size}}};
		drawData_.canvasOffset = {pos.x, pos.y};
		drawData_.canvasSize = {avail.x, avail.y};
		drawData_.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		drawData_.useW = false;
		drawData_.scale = 1.f;
		drawData_.drawFrustum = false;
		drawData_.clear = true;
		drawData_.mat = nytl::identity<4, float>();

		auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
			auto* self = static_cast<VertexViewer*>(cmd->UserCallbackData);
			self->imGuiDraw(self->drawData_);
		};

		ImGui::GetWindowDrawList()->AddCallback(cb, this);
		ImGui::InvisibleButton("Canvas", avail);
		updateInput(dt);
	}

	ImGui::EndChild();
}

AABB3f bounds(const AABB3f& a, const AABB3f& b) {
	auto inf = 99999999.f; // std::numeric_limits<float>::infinity();
	if(a.pos.x >= inf)
	{
		return b;
	}
	if(b.pos.x >= inf)
	{
		return a;
	}

	auto start = nytl::vec::cw::min(a.pos - a.extent, b.pos - b.extent);
	auto end = nytl::vec::cw::max(a.pos + a.extent, b.pos + b.extent);

	return {0.5f * (start + end), 0.5f * (end - start)};
}

AABB3f transform(const AABB3f& a, const Mat4f& transform) {
	using nytl::vec::cw::operators::operator*;
	using nytl::vec::cw::min;
	using nytl::vec::cw::max;

	auto p0 = multPos(transform, a.pos + Vec3f{1.f, -1.f, -1.f} * a.extent);
	auto p1 = multPos(transform, a.pos + Vec3f{-1.f, 1.f, -1.f} * a.extent);
	auto p2 = multPos(transform, a.pos + Vec3f{-1.f, -1.f, 1.f} * a.extent);
	auto p3 = multPos(transform, a.pos + Vec3f{1.f, 1.f, -1.f} * a.extent);
	auto p4 = multPos(transform, a.pos + Vec3f{1.f, -1.f, 1.f} * a.extent);
	auto p5 = multPos(transform, a.pos + Vec3f{-1.f, 1.f, 1.f} * a.extent);
	auto p6 = multPos(transform, a.pos + Vec3f{1.f, 1.f, 1.f} * a.extent);
	auto p7 = multPos(transform, a.pos + Vec3f{-1.f, -1.f, -1.f} * a.extent);

	auto start = p0;
	auto end = p0;

	start = min(start, p1); end = max(end, p1);
	start = min(start, p2); end = max(end, p2);
	start = min(start, p3); end = max(end, p3);
	start = min(start, p4); end = max(end, p4);
	start = min(start, p5); end = max(end, p5);
	start = min(start, p6); end = max(end, p6);
	start = min(start, p7); end = max(end, p7);

	return {0.5f * (start + end), 0.5f * (end - start)};
}

void VertexViewer::displayInstances(Draw& draw, const AccelInstances& instances,
		float dt, std::function<AccelStructStatePtr(u64)> blasResolver) {
	ZoneScoped;

	if(ImGui::Button("Recenter")) {
		auto inf = 999999999.f; // std::numeric_limits<float>::infinity();
		AABB3f vertBounds {inf, inf, inf, 0.f, 0.f, 0.f};

		for(auto& ini : instances.instances) {
			if(!ini.accelerationStructureReference) {
				continue;
			}
			auto blasState = blasResolver(ini.accelerationStructureReference);
			// TODO: check that the build is finished?
			//   hm, keep last built version in blas as well for
			//   resource viewer and show that instead when built is not
			//   finished for most current one?
			if(!blasState || blasState->data.index() != 0u) {
				continue;
			}
			auto& tris = std::get<0>(blasState->data);
			vertBounds = bounds(vertBounds, transform(bounds(tris),
				toMat4f(ini.transform)));
		}

		centerCamOnBounds(vertBounds);
	}

	if(ImGui::BeginChild("vertexViewer")) {
		auto avail = ImGui::GetContentRegionAvail();
		auto pos = ImGui::GetCursorScreenPos();

		// we statically know the single binding and attribute
		drawData_.clear = true;
		auto& vinput = drawData_.vertexInput;
		vinput.bindings.resize(1);
		vinput.attribs.resize(1);

		vinput.bindings[0] = {
			0u, sizeof(Vec4f), VK_VERTEX_INPUT_RATE_VERTEX,
		};

		vinput.attribs[0] = {
			0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u,
		};

		drawDatas_.clear();
		drawDatas_.reserve(instances.instances.size());

		for(auto& ini : instances.instances) {
			ZoneScopedN("ini");
			if(!ini.accelerationStructureReference) {
				continue;
			}

			auto blasState = blasResolver(ini.accelerationStructureReference);
			if(!blasState || blasState->data.index() != 0u) {
				continue;
			}

			dlg_assert(blasState->built);
			auto& tris = std::get<0>(blasState->data);

			// TODO: we should multiple draw calls instead of just batching
			// everything together. E.g. to visualize different geometry flags
			// or to color them differently.
			auto drawCount = 0u;
			for(auto& geom : tris.geometries) {
				drawCount += geom.triangles.size() * 3;
			}

			// TODO: inefficient, should batch it via drawData_ somehow into
			// one call
			auto& data = drawDatas_.emplace_back();
			data = drawData_;
			data.cb = draw.cb;
			data.params = {};
			data.indexBuffer = {};
			data.canvasOffset = {pos.x, pos.y};
			data.canvasSize = {avail.x, avail.y};
			data.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			data.useW = false;
			data.scale = 1.f;
			data.drawFrustum = false;
			data.params.drawCount = drawCount;
			data.vertexBuffers = {{{blasState->buffer.buf, 0u, blasState->buffer.size}}};
			data.self = this;
			data.mat = toMat4f(ini.transform);

			auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
				auto* data = static_cast<DrawData*>(cmd->UserCallbackData);
				data->self->imGuiDraw(*data);
			};

			ImGui::GetWindowDrawList()->AddCallback(cb, &data);
			drawData_.clear = false;
		}

		ImGui::InvisibleButton("Canvas", avail);
		updateInput(dt);
	}

	ImGui::EndChild();
}

} // namespace vil
