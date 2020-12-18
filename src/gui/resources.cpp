#include <gui/resources.hpp>
#include <gui/gui.hpp>
#include <handles.hpp>
#include <util.hpp>
#include <imguiutil.hpp>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <spirv_reflect.h>
#include <map>

namespace fuen {

ResourceGui::~ResourceGui() {
	if(image_.view) {
		gui_->dev().dispatch.DestroyImageView(gui_->dev().handle,
			image_.view, nullptr);
	}
}

void ResourceGui::drawMemoryResDesc(Draw&, MemoryResource& res) {
	if(res.memory) {
		ImGui::Text("Bound to memory ");
		ImGui::SameLine();
		auto label = name(*res.memory);
		if(ImGui::Button(label.c_str())) {
			select(*res.memory);
		}

		ImGui::SameLine();
		imGuiText(" (offset {}, size {})",
			(unsigned long) res.allocationOffset,
			(unsigned long) res.allocationSize);
	}
}

void ResourceGui::drawDesc(Draw& draw, Image& image) {
	ImGui::Text("%s", name(image).c_str());
	ImGui::Spacing();

	auto& dev = gui_->dev();
	bool recreateView = false;
	bool canHaveView =
		!image.swapchain &&
		image.pendingLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
		image.samplerType != Image::SamplerType::none;
	if(image_.object != &image) {
		if(image_.view) {
			dev.dispatch.DestroyImageView(dev.handle, image_.view, nullptr);
		}

		image_ = {};
		image_.object = &image;
	}

	recreateView |= (!image_.view && canHaveView);
	recreateView |= image_.view && (
		(image_.newSubres.aspectMask != image_.subres.aspectMask) ||
		(image_.newSubres.baseArrayLayer != image_.subres.baseArrayLayer) ||
		(image_.newSubres.baseMipLevel != image_.subres.baseMipLevel));

	if(recreateView) {
		if(!image_.view) {
			if(FormatIsDepthAndStencil(image.ci.format) || FormatIsDepthOnly(image.ci.format)) {
				image_.subres.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			} else if(FormatIsStencilOnly(image.ci.format)) {
				image_.subres.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
			} else {
				image_.subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			}

			image_.subres.levelCount = 1;
			image_.subres.layerCount = 1;
			image_.newSubres = image_.subres;

			auto numComponents = FormatChannelCount(image_.object->ci.format);
			if(numComponents == 1) {
				image_.draw.flags |= DrawGuiImage::flagMaskR | DrawGuiImage::flagGrayscale;
			} else {
				for(auto i = 0u; i < numComponents; ++i) {
					image_.draw.flags |= (1u << i);
				}
			}
		} else if(image_.view) {
			dev.dispatch.DestroyImageView(dev.handle, image_.view, nullptr);
		}

		// TODO: check if images supports sampling in the first place!
		if(!image.swapchain) {
			auto getViewType = [&]{
				switch(image.ci.imageType) {
					case VK_IMAGE_TYPE_1D:
						image_.draw.type = DrawGuiImage::e1d;
						return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
					case VK_IMAGE_TYPE_2D:
						image_.draw.type = DrawGuiImage::e2d;
						return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
					case VK_IMAGE_TYPE_3D:
						image_.draw.type = DrawGuiImage::e3d;
						return VK_IMAGE_VIEW_TYPE_3D;
					default:
						dlg_error("Unsupported image type");
						return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
				}
			};

			VkImageViewCreateInfo ivi {};
			ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			ivi.image = image.handle;
			ivi.viewType = getViewType();
			ivi.format = image.ci.format;
			ivi.subresourceRange = image_.newSubres;

			VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &image_.view));
			nameHandle(dev, image_.view, "ResourceGui:image_.view");

			VkDescriptorImageInfo dsii {};
			dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			dsii.imageView = image_.view;
			dsii.sampler = image.samplerType == Image::SamplerType::linear ?
				dev.renderData->linearSampler :
				dev.renderData->nearestSampler;

			VkWriteDescriptorSet write {};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.descriptorCount = 1u;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.dstSet = draw.dsSelected;
			write.pImageInfo = &dsii;

			dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);
		}

		image_.subres = image_.newSubres;
	}

	// info
	const auto& ci = image.ci;
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
	ImGui::Text("%s", vk::flagNames(VkImageUsageFlagBits(ci.usage)).c_str());
	ImGui::Text("%s", vk::name(ci.tiling));
	ImGui::Text("%s", vk::name(ci.samples));
	ImGui::Text("%s", vk::name(ci.imageType));
	ImGui::Text("%s", vk::flagNames(VkImageUsageFlagBits(ci.flags)).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResDesc(draw, image);

	ImGui::Spacing();
	ImGui::Text("Image Views:");

	for(auto* view : image.views) {
		ImGui::Bullet();
		if(ImGui::Button(name(*view).c_str())) {
			select(*view);
		}
	}

	// content
	if(image.swapchain) {
		ImGui::Text("Image can't be displayed since it's a swapchain image");
	} else if(image.samplerType == Image::SamplerType::none) {
		ImGui::Text("Image can't be displayed since its format does not support sampling");
	} else if(image.pendingLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
		// TODO: well, we could still try to display it.
		// But we have to modify our barrier logic a bit.
		// And should probably at least output a warning here that it's in
		// undefined layout and therefore may contain garbage, nothing
		// we can do about that (well, once again not entirely true, we could
		// prevent this invalidation by hooking into state transitions
		// and prevent images from being put into undefined layout; always
		// storing in renderpass and so on. But that's really something
		// for waaaay later, i'm already wondering wth i'm doing with my life
		// writing this).
		ImGui::Text("Image can't be displayed since it's in undefined layout, has undefined content");
	} else {
		dlg_assert(image_.view);

		ImGui::Spacing();
		ImGui::Spacing();

        ImVec2 pos = ImGui::GetCursorScreenPos();

		float aspect = float(image.ci.extent.width) / image.ci.extent.height;

		// TODO: this logic might lead to problems for 1xHUGE images
		float regW = ImGui::GetContentRegionAvail().x - 20.f;
		float regH = regW / aspect;

		ImGui::Image((void*) &image_.draw, {regW, regH});

		// Taken pretty much just from the imgui demo
		auto& io = gui_->imguiIO();
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			float region_sz = 64.0f;
			float region_x = io.MousePos.x - pos.x - region_sz * 0.5f;
			float region_y = io.MousePos.y - pos.y - region_sz * 0.5f;
			float zoom = 4.0f;
			if (region_x < 0.0f) { region_x = 0.0f; }
			else if (region_x > regW - region_sz) { region_x = regW - region_sz; }
			if (region_y < 0.0f) { region_y = 0.0f; }
			else if (region_y > regH - region_sz) { region_y = regH - region_sz; }
			ImGui::Text("Min: (%.2f, %.2f)", region_x, region_y);
			ImGui::Text("Max: (%.2f, %.2f)", region_x + region_sz, region_y + region_sz);
			ImVec2 uv0 = ImVec2((region_x) / regW, (region_y) / regH);
			ImVec2 uv1 = ImVec2((region_x + region_sz) / regW, (region_y + region_sz) / regH);
			ImGui::Image((void*) &image_.draw, ImVec2(region_sz * zoom, region_sz * zoom), uv0, uv1);
			ImGui::EndTooltip();
		}

		// Row 1: components
		auto numComponents = FormatChannelCount(image_.object->ci.format);

		ImGui::CheckboxFlags("R", &image_.draw.flags, DrawGuiImage::flagMaskR);
		if(numComponents > 1) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("G", &image_.draw.flags, DrawGuiImage::flagMaskG);
		}
		if(numComponents > 2) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("B", &image_.draw.flags, DrawGuiImage::flagMaskB);
		}
		if(numComponents > 3) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("A", &image_.draw.flags, DrawGuiImage::flagMaskA);
		}

		ImGui::SameLine();
		ImGui::CheckboxFlags("Gray", &image_.draw.flags, DrawGuiImage::flagGrayscale);

		// Row 2: layer and mip
		if(image.ci.extent.depth > 1) {
			ImGui::SliderFloat("slice", &image_.draw.layer, 0, image.ci.extent.depth - 1);
		} else if(image.ci.arrayLayers > 1) {
			int layer = image_.newSubres.baseArrayLayer;
			ImGui::SliderInt("Layer", &layer, 0, image.ci.arrayLayers - 1);
			image_.newSubres.baseArrayLayer = layer;
		}

		if(image.ci.mipLevels > 1) {
			ImGui::SameLine();
			int mip = image_.newSubres.baseMipLevel;
			ImGui::SliderInt("Mip", &mip, 0, image.ci.mipLevels - 1);
			image_.newSubres.baseMipLevel = mip;
		}

		// Row 3: min/max values
		ImGui::DragFloat("Min", &image_.draw.minValue, 0.01);
		ImGui::DragFloat("Max", &image_.draw.maxValue, 0.01);
	}

	// TODO: display pending layout?
}

void ResourceGui::drawDesc(Draw& draw, Buffer& buffer) {
	if(buffer_.handle != &buffer) {
		// TODO: remember used layouts per-buffer?
		// But would be nice to have mechanism for that across multiple starts
		buffer_ = {};
		buffer_.handle = &buffer;
	}

	ImGui::Text("%s", name(buffer).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);

	ImGui::SetColumnWidth(0, 100);

	ImGui::Text("Size");
	ImGui::Text("Usage");

	ImGui::NextColumn();

	auto& ci = buffer.ci;
	imGuiText("{}", ci.size);
	imGuiText("{}", vk::flagNames(VkBufferUsageFlagBits(ci.usage)).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResDesc(draw, buffer);

	// content
	// we are using a child window to avoid column glitches
	if(!buffer_.lastRead.empty() && ImGui::BeginChild("BufContent")) {
		imGuiText("Content");
		ImGui::Spacing();

		if(imGuiTextMultiline("Layout", buffer_.layoutText)) {
			buffer_.layout.clear();

			constexpr auto whitespace = "\n\t\f\r\v "; // as by std::isspace
			constexpr auto whitespaceSemic = "\n\t\f\r\v; ";

			// TODO: extend
			const std::unordered_map<std::string_view, VkFormat> layoutMap = {
				{"float", VK_FORMAT_R32_SFLOAT},
				{"f32", VK_FORMAT_R32_SFLOAT},
				{"vec1", VK_FORMAT_R32_SFLOAT},
				{"float1", VK_FORMAT_R32_SFLOAT},
				{"vec2", VK_FORMAT_R32G32_SFLOAT},
				{"float2", VK_FORMAT_R32G32_SFLOAT},
				{"vec3", VK_FORMAT_R32G32B32_SFLOAT},
				{"float3", VK_FORMAT_R32G32B32_SFLOAT},
				{"vec4", VK_FORMAT_R32G32B32A32_SFLOAT},
				{"float4", VK_FORMAT_R32G32B32A32_SFLOAT},

				{"half", VK_FORMAT_R16_SFLOAT},
				{"f16", VK_FORMAT_R16_SFLOAT},
				{"float16_t", VK_FORMAT_R16_SFLOAT},
				{"half1", VK_FORMAT_R16_SFLOAT},
				{"f16vec1", VK_FORMAT_R16_SFLOAT},
				{"f16vec2", VK_FORMAT_R16G16_SFLOAT},
				{"half2", VK_FORMAT_R16G16_SFLOAT},
				{"f16vec3", VK_FORMAT_R16G16B16_SFLOAT},
				{"half3", VK_FORMAT_R16G16B16_SFLOAT},
				{"f16vec4", VK_FORMAT_R16G16B16A16_SFLOAT},
				{"half4", VK_FORMAT_R16G16B16A16_SFLOAT},
			};

			auto lt = std::string_view(buffer_.layoutText);
			for(auto end = lt.find(';'); end != lt.npos; end = lt.find(';')) {
				auto start = lt.find_first_not_of(whitespace);
				auto elem = lt.substr(start);

				auto sep = elem.find_first_of(whitespaceSemic);
				auto type = elem.substr(0, sep);
				auto it = layoutMap.find(type);
				if(it == layoutMap.end()) {
					dlg_error("Invalid buffer layout identifier {}", type);
					break;
				}

				std::string_view ident = type;
				if(sep != end) {
					ident = elem.substr(sep + 1);
					start = ident.find_first_not_of(whitespace);
					ident = ident.substr(start);
					sep = ident.find_first_of(whitespaceSemic);
					ident = ident.substr(0, sep);
				}

				buffer_.layout.push_back({std::string(ident), it->second});
				lt = lt.substr(end + 1);
			}

			if(!lt.empty()) {
				auto start = lt.find_first_not_of(whitespace);
				lt = lt.substr(start);
			}

			dlg_assertm(lt.empty(), "Invalid buffer layout ending: {}", lt);
		}

		if(!buffer_.layout.empty()) {
			auto data = span<const std::byte>(buffer_.lastRead);

			// TODO: offset & scrolling!
			constexpr auto maxSize = 1024;
			if(data.size() > maxSize) {
				data = data.first(maxSize);
			}

			ImGui::Columns(u32(buffer_.layout.size()));
			for(auto& [name, _] : buffer_.layout) {
				imGuiText("{}", name);
				ImGui::NextColumn();
			}

			ImGui::Separator();

			auto done = false;
			while(!done) {
				for(auto& [name, format] : buffer_.layout) {
					if(data.size() < FormatTexelSize(format)) {
						done = true;
						break;
					}

					auto val = read(format, data);
					auto cc = FormatChannelCount(format);
					dlg_assert(cc <= 4);

					std::string text;
					for(auto i = 0u; i < cc; ++i) {
						text += dlg::format("{} ", val[i]);
					}

					imGuiText("{}", text);
					ImGui::NextColumn();
				}
			}

			dlg_assertm(data.empty(), "Leftover data in buffer");
			ImGui::Columns();
		}

		ImGui::EndChild();
	}
}

void ResourceGui::drawDesc(Draw&, Sampler& sampler) {
	ImGui::Text("%s", name(sampler).c_str());
	ImGui::Spacing();
	const auto& ci = sampler.ci;

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

void ResourceGui::drawDesc(Draw&, DescriptorSet&) {
	ImGui::Text("TODO");
}

void ResourceGui::drawDesc(Draw&, DescriptorPool&) {
	ImGui::Text("TODO");
}

void ResourceGui::drawDesc(Draw&, DescriptorSetLayout& dsl) {
	ImGui::Text("%s", name(dsl).c_str());
	ImGui::Spacing();

	ImGui::Text("Bindings");

	for(auto& binding : dsl.bindings) {
		// TODO: immutable samplers
		if(binding.descriptorCount > 1) {
			ImGui::BulletText("%s[%d] in (%s)",
				vk::name(binding.descriptorType),
				binding.descriptorCount,
				vk::flagNames(VkShaderStageFlagBits(binding.stageFlags)).c_str());
		} else {
			ImGui::BulletText("%s in (%s)",
				vk::name(binding.descriptorType),
				vk::flagNames(VkShaderStageFlagBits(binding.stageFlags)).c_str());
		}
	}
}

void ResourceGui::drawDesc(Draw&, GraphicsPipeline& pipe) {
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

	if(ImGui::Button(name(*pipe.layout).c_str())) {
		select(*pipe.layout);
	}
	// TODO: allow to display RenderPassDesc
	// if(ImGui::Button(name(*pipe.renderPass).c_str())) {
	// 	select(*pipe.renderPass);
	// }
	ImGui::Text("%d", pipe.subpass);

	ImGui::Columns();
	ImGui::Separator();

	// rasterization
	const auto& rastInfo = pipe.rasterizationState;

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

	ImGui::Text("%s", vk::flagNames(VkCullModeFlagBits(rastInfo.cullMode)).c_str());
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
		ImGui::Text("%s", vk::name(pipe.inputAssemblyState.topology));

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
					vk::name(binding.inputRate), binding.stride);

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
						attrib.location, attrib.offset, vk::name(attrib.format));
				}

				ImGui::Unindent();
			}

			ImGui::Separator();
		}
	}

	if(!pipe.dynamicState.empty()) {
		ImGui::Text("Dynamic states");

		for(auto& dynState : pipe.dynamicState) {
			ImGui::BulletText("%s", vk::name(dynState));
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

			ImGui::Text("%s", vk::name(pipe.multisampleState.rasterizationSamples));
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
				ImGui::Text("%s", vk::name(pipe.depthStencilState.depthCompareOp));

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
					ImGui::TreePop();
				}

				if(ImGui::TreeNode("Stencil Back")) {
					printStencilState(pipe.depthStencilState.back);
					ImGui::TreePop();
				}
			}
			*/

			ImGui::Columns();
			ImGui::Separator();
		}
	}

	ImGui::Text("Stages");
	for(auto& stage : pipe.stages) {
		if(ImGui::TreeNode(&stage, "%s", vk::name(stage.stage))) {
			ImGui::Text("Entry Point: %s", stage.entryPoint.c_str());
			// TODO: spec data


			auto& refl = nonNull(stage.spirv->reflection.get());
			auto& entryPoint = nonNull(spvReflectGetEntryPoint(&refl, stage.entryPoint.c_str()));

			// TODO: shader module info
			// - source language
			// - push constant blocks?
			// - all entry points?
			// - all descriptor sets?

			ImGui::Text("Entry Point %s:", entryPoint.name);
			ImGui::Text("Input variables");
			for(auto i = 0u; i < entryPoint.input_variable_count; ++i) {
				auto& iv = entryPoint.input_variables[i];

				if(ImGui::TreeNode(&iv, "%d: %s", iv.location, iv.name)) {
					asColumns2({{
						{"Format", "{}", vk::name(VkFormat(iv.format))},
						{"Storage", "{}", iv.storage_class},
					}});

					ImGui::TreePop();
				}
			}

			ImGui::Text("Output variables");
			for(auto i = 0u; i < entryPoint.output_variable_count; ++i) {
				auto& ov = entryPoint.output_variables[i];

				if(ImGui::TreeNode(&ov, "%d: %s", ov.location, ov.name)) {
					asColumns2({{
						{"Format", "{}", vk::name(VkFormat(ov.format))},
						{"Storage", "{}", ov.storage_class},
					}});

					ImGui::TreePop();
				}
			}

			ImGui::Text("Descriptor Sets");
			for(auto i = 0u; i < entryPoint.descriptor_set_count; ++i) {
				auto& ds = entryPoint.descriptor_sets[i];

				if(ImGui::TreeNode(&ds, "Set %d", ds.set)) {
					for(auto b = 0u; b < ds.binding_count; ++b) {
						auto& binding = *ds.bindings[b];

						std::string name = dlg::format("{}: {}",
							binding.binding,
							vk::name(VkDescriptorType(binding.descriptor_type)));
						if(binding.count > 1) {
							name += dlg::format("[{}]", binding.count);
						}
						name += " ";
						name += binding.name;

						ImGui::BulletText("%s", name.c_str());
					}

					ImGui::TreePop();
				}
			}

			// TODO: only show for compute shaders
			ImGui::Text("Workgroup size: %d %d %d",
				entryPoint.local_size.x,
				entryPoint.local_size.y,
				entryPoint.local_size.z);

			/*
			if(ImGui::Button("Open in Vim")) {
				namespace fs = std::filesystem;

				auto fileName = dlg::format("fuencaliente.{}.spv", (std::uint64_t) stage.spirv.get());
				auto tmpPath = fs::temp_directory_path() / fileName;

				bool launch = false;

				{
					auto of = std::ofstream(tmpPath, std::ios::out | std::ios::binary);
					if(of.is_open()) {
						of.write((const char*) stage.spirv->spv.data(), stage.spirv->spv.size() * 4);
						of.flush();
						launch = true;
					}
				}

				// ugh, not exactly beautiful, i know
				if(launch) {
					auto cmd = dlg::format("termite -e 'nvim {}' &", tmpPath);
					dlg_info("cmd: {}", cmd);
					std::system(cmd.c_str());
				}

				// TODO: we should probably delete the file somehow...
			}
			*/

			// TODO: used push constants

			ImGui::TreePop();
		}
	}

	// TODO: color blend state
	// TODO: tesselation
}

void ResourceGui::drawDesc(Draw&, ComputePipeline&) {
	ImGui::Text("TODO");
}

void ResourceGui::drawDesc(Draw&, PipelineLayout& pipeLayout) {
	ImGui::Text("%s", name(pipeLayout).c_str());
	ImGui::Spacing();

	if(!pipeLayout.pushConstants.empty()) {
		ImGui::Text("Push Constants");
		for(auto& pcr : pipeLayout.pushConstants) {
			ImGui::Bullet();
			ImGui::Text("Offset %d, Size %d, in %s", pcr.offset, pcr.size,
				vk::flagNames(VkShaderStageFlagBits(pcr.stageFlags)).c_str());
		}
	}

	ImGui::Text("Descriptor Set Layouts");
	for(auto* ds : pipeLayout.descriptors) {
		ImGui::Bullet();
		if(ImGui::Button(name(*ds).c_str())) {
			select(*ds);
		}
	}
}
void ResourceGui::drawDesc(Draw&, CommandPool& cp) {
	ImGui::Text("%s", name(cp).c_str());
	ImGui::Spacing();

	// TODO: display information

	for(auto& cb : cp.cbs) {
		if(ImGui::Button(name(*cb).c_str())) {
			select(*cb);
		}
	}
}

void ResourceGui::drawDesc(Draw&, DeviceMemory& mem) {
	ImGui::Text("%s", name(mem).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);

	ImGui::Text("Size");
	ImGui::Text("Type Index");

	// data
	ImGui::NextColumn();

	imGuiText("{}", mem.size);
	imGuiText("{}", mem.typeIndex);

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	ImGui::Text("Bound Resources:");

	ImGui::Columns(3);
	ImGui::SetColumnWidth(0, 100);
	ImGui::SetColumnWidth(1, 300);

	for(auto& resource : mem.allocations) {
		imGuiText("{}: ", resource->allocationOffset);

		ImGui::NextColumn();

		if(resource->objectType == VK_OBJECT_TYPE_BUFFER) {
			Buffer& buffer = static_cast<Buffer&>(*resource);
			auto label = name(buffer);
			ImGui::Button(label.c_str());
		} else if(resource->objectType == VK_OBJECT_TYPE_IMAGE) {
			Image& img = static_cast<Image&>(*resource);
			auto label = name(img);
			ImGui::Button(label.c_str());
		}

		ImGui::NextColumn();
		imGuiText("size {}", resource->allocationSize);

		ImGui::NextColumn();
	}

	ImGui::Columns();
}

void ResourceGui::drawDesc(Draw&, CommandBuffer& cb) {
	ImGui::Text("%s", name(cb).c_str());
	ImGui::Spacing();

	// TODO: more info about cb

	ImGui::Text("Pool: ");
	ImGui::SameLine();
	if(ImGui::Button(name(*cb.pool).c_str())) {
		select(*cb.pool);
	}

	auto stateName = [](auto state) {
		switch(state) {
			case CommandBuffer::State::executable: return "executable";
			case CommandBuffer::State::invalid: return "invalid";
			case CommandBuffer::State::initial: return "initial";
			case CommandBuffer::State::recording: return "recording";
			default: return "unknonw";
		}
	};

	ImGui::Text("State: %s", stateName(cb.state));

	// maybe show commands inline (in tree node)
	// and allow via button to switch to cb viewer?
	if(ImGui::Button("View Content")) {
		gui_->selectCb(cb);
	}
}

void imguiPrintRange(u32 base, u32 count) {
	if(count > 1) {
		ImGui::Text("[%d, %d]", base, base + count - 1);
	} else {
		ImGui::Text("%d", base);
	}
}

void ResourceGui::drawDesc(Draw&, ImageView& view) {
	ImGui::Text("%s", name(view).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);
	auto& ci = view.ci;

	ImGui::Text("Image");
	ImGui::Text("Type");
	ImGui::Text("Layers");
	ImGui::Text("Levels");
	ImGui::Text("Aspect");
	ImGui::Text("Format");
	ImGui::Text("Flags");

	// data
	ImGui::NextColumn();

	if(view.img) {
		if(ImGui::Button(name(*view.img).c_str())) {
			select(*view.img);
		}
	} else {
		ImGui::Text("Associated Image was destroyed");
	}

	ImGui::Text("%s", vk::name(ci.viewType));
	imguiPrintRange(ci.subresourceRange.baseArrayLayer, ci.subresourceRange.layerCount);
	imguiPrintRange(ci.subresourceRange.baseMipLevel, ci.subresourceRange.levelCount);
	ImGui::Text("%s", vk::flagNames(VkImageAspectFlagBits(ci.subresourceRange.aspectMask)).c_str());
	ImGui::Text("%s", vk::name(ci.format));
	ImGui::Text("%s", vk::flagNames(VkImageViewCreateFlagBits(ci.flags)).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	if(!view.fbs.empty()) {
		ImGui::Text("Framebuffers:");

		for(auto* fb : view.fbs) {
			ImGui::Bullet();
			if(ImGui::Button(name(*fb).c_str())) {
				select(*fb);
			}
		}
	}
}

void ResourceGui::drawDesc(Draw&, ShaderModule&) {
	ImGui::Text("TODO");
}

void ResourceGui::drawDesc(Draw&, Framebuffer& fb) {
	ImGui::Text("%s", name(fb).c_str());
	ImGui::Spacing();

	asColumns2({{
		{"Width", "{}", fb.width},
		{"Height", "{}", fb.height},
		{"Layers", "{}", fb.layers},
	}});

	// Resource references
	ImGui::Spacing();
	ImGui::Text("Attachments:");

	for(auto* view : fb.attachments) {
		ImGui::Bullet();
		if(ImGui::Button(name(*view).c_str())) {
			select(*view);
		}
	}
}

void ResourceGui::drawDesc(Draw&, RenderPass& rp) {
	ImGui::Text("%s", name(rp).c_str());
	ImGui::Spacing();

	// info
	// attachments
	for(auto i = 0u; i < rp.desc->attachments.size(); ++i) {
		const auto& att = rp.desc->attachments[i];
		if(ImGui::TreeNode(&rp.desc->attachments[i], "Attachment %d: %s", i, vk::name(att.format))) {
			asColumns2({{
				{"Samples", "{}", vk::name(att.samples)},
				{"Initial Layout", "{}", vk::name(att.initialLayout)},
				{"Final Layout", "{}", vk::name(att.finalLayout)},
				{"Flags", "{}", vk::flagNames(VkAttachmentDescriptionFlagBits(att.flags))},
				{"Load Op", "{}", vk::name(att.loadOp)},
				{"Store Op", "{}", vk::name(att.storeOp)},
				{"Stencil Load Op", "{}", vk::name(att.stencilLoadOp)},
				{"Stencil Store Op", "{}", vk::name(att.stencilStoreOp)},
			}});

			ImGui::TreePop();
		}
	}

	// subpasses
	for(auto i = 0u; i < rp.desc->subpasses.size(); ++i) {
		const auto& subp = rp.desc->subpasses[i];
		if(ImGui::TreeNode(&rp.desc->subpasses[i], "Subpass %d", i)) {
			asColumns2({{
				{"Pipeline Bind Point", "{}", vk::name(subp.pipelineBindPoint)},
				{"Flags", "{}", vk::flagNames(VkSubpassDescriptionFlagBits(subp.flags)).c_str()},
			}});

			ImGui::Separator();
			if(subp.colorAttachmentCount) {
				ImGui::Text("Color Attachments:");
				for(auto c = 0u; c < subp.colorAttachmentCount; ++c) {
					auto& att = subp.pColorAttachments[c];
					ImGui::BulletText("%d, %s", att.attachment, vk::name(att.layout));
				}
			}

			if(subp.inputAttachmentCount) {
				ImGui::Text("Input Attachments:");
				for(auto c = 0u; c < subp.inputAttachmentCount; ++c) {
					auto& att = subp.pInputAttachments[c];
					ImGui::BulletText("%d, %s", att.attachment, vk::name(att.layout));
				}
			}

			if(subp.pDepthStencilAttachment) {
				auto& att = *subp.pDepthStencilAttachment;
				ImGui::Text("DepthStencil Attachment: %d, %s", att.attachment,
					vk::name(att.layout));
			}

			if(subp.preserveAttachmentCount) {
				ImGui::Text("Preserve Attachments: ");
				for(auto c = 0u; c < subp.preserveAttachmentCount; ++c) {
					ImGui::SameLine();
					ImGui::Text("%d ", subp.pPreserveAttachments[c]);
				}
			}

			ImGui::TreePop();
		}
	}

	// TODO: dependencies
}

void ResourceGui::drawDesc(Draw&, Event&) {
}
void ResourceGui::drawDesc(Draw&, Semaphore&) {
}
void ResourceGui::drawDesc(Draw&, Fence&) {
}
void ResourceGui::drawDesc(Draw&, BufferView&) {
}
void ResourceGui::drawDesc(Draw&, QueryPool&) {
}

void ResourceGui::draw(Draw& draw) {
	// search settings
	ImGui::Columns(2);
	ImGui::BeginChild("Search settings", {0.f, 50.f});

	imGuiTextInput("Search", search_);

	auto filters = {
		0,
		int(VK_OBJECT_TYPE_IMAGE),
		int(VK_OBJECT_TYPE_COMMAND_BUFFER),
		int(VK_OBJECT_TYPE_PIPELINE),
		int(VK_OBJECT_TYPE_SAMPLER),
	};

	auto filterName = filter_ == 0 ? "all" : vk::name(VkObjectType(filter_));
	if(ImGui::BeginCombo("Filter", filterName)) {
		for(auto& filter : filters) {
			auto name = filter == 0 ? "all" : vk::name(VkObjectType(filter));
			if(ImGui::Selectable(name)) {
				this->filter_ = filter;
			}
		}

		ImGui::EndCombo();
	}

	ImGui::Separator();
	ImGui::EndChild();

	// resource list
	ImGui::BeginChild("Resource List", {0.f, 0.f});

	auto displayResources = [&](auto& resMap) {
		for(auto& entry : resMap.map) {
			auto label = name(*entry.second);

			// TODO: better search
			if(!search_.empty() && label.find(search_) == label.npos) {
				continue;
			}

			if(filter_ != 0 && int(entry.second->objectType) != filter_) {
				// Break instead of continue since no object
				// in this map should be displayed. TODO: kinda ugly...
				break;
			}

			ImGui::PushID(entry.second.get());
			if(ImGui::Button(label.c_str())) {
				select(*entry.second.get());
			}

			ImGui::PopID();
		}
	};

	auto& dev = gui_->dev();
	displayResources(dev.images);
	displayResources(dev.imageViews);
	displayResources(dev.samplers);
	displayResources(dev.framebuffers);
	displayResources(dev.renderPasses);
	displayResources(dev.buffers);
	displayResources(dev.deviceMemories);
	displayResources(dev.commandBuffers);
	displayResources(dev.commandPools);
	displayResources(dev.dsPools);
	displayResources(dev.descriptorSets);
	displayResources(dev.dsLayouts);
	displayResources(dev.graphicsPipes);
	displayResources(dev.computePipes);
	displayResources(dev.pipeLayouts);
	displayResources(dev.shaderModules);
	displayResources(dev.events);
	displayResources(dev.semaphores);
	displayResources(dev.fences);
	displayResources(dev.queryPools);
	displayResources(dev.bufferViews);

	ImGui::EndChild();

	// resource view
	ImGui::NextColumn();
	ImGui::BeginChild("Resource View", {0.f, 0.f});

	std::visit(Visitor{
		[&](std::monostate) {},
		[&](auto* selected) {
			ImGui::PushID(selected);
			drawDesc(draw, *selected);
			ImGui::PopID();
		}
	}, handle_);

	ImGui::EndChild();
	ImGui::Columns();
}

void ResourceGui::destroyed(const Handle& handle) {
	auto same = std::visit(Visitor{
		[&](std::monostate) {
			return false;
		}, [&](auto& selected) {
			return selected == &handle;
		}
	}, handle_);

	auto& dev = gui_->dev();
	if(same) {
		handle_ = {};
		if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
			if(image_.view) {
				dev.dispatch.DestroyImageView(dev.handle, image_.view, nullptr);
			}

			image_ = {};
		}
	}
}

} // namespace fuen
