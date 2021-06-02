#include <gui/resources.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <device.hpp>
#include <handles.hpp>
#include <util/util.hpp>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <util/spirv_reflect.h>
#include <map>

namespace vil {

// NOTE: use something like this? But this actively hides information,
// e.g. maybe the user wants to see the *exact* size, not just 122MB.
std::string formatSize(u64 size) {
	if(size > 10'000'000) {
		return dlg::format("{} MB", size / 1000 * 1000);
	}

	if(size > 10'000) {
		return dlg::format("{} KB", size / 1000);
	}

	return dlg::format("{} B", size);
}

std::string sepfmt(u64 size) {
	std::string ret;
	if(!size) {
		return "0";
	}

	char nums[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

	auto poten = u64(1u);
	auto counter = 0u;
	while(size) {
		if(++counter == 3u) {
			counter = 0u;
			ret.insert(0, 1, '\'');
		}

		auto rest = (size % (10 * poten)) / poten;
		size -= rest * poten;
		ret.insert(0, 1, nums[rest]);

		poten *= 10;
	}

	return ret;
}

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

		refButtonExpect(*gui_, res.memory);

		ImGui::SameLine();
		imGuiText(" (offset {}, size {})",
			sepfmt(res.allocationOffset),
			sepfmt(res.allocationSize));
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
		image.allowsNearestSampling &&
		image.ci.samples == VK_SAMPLE_COUNT_1_BIT;
	if(image_.object != &image) {
		if(image_.view) {
			dev.dispatch.DestroyImageView(dev.handle, image_.view, nullptr);
		}

		image_ = {};
		image_.object = &image;
	}

	recreateView |= (!image_.view && canHaveView);
	recreateView |= image_.view && (
		(image_.aspect != image_.draw.aspect) ||
		(image_.level != image_.draw.level));

	if(recreateView) {
		if(!image_.view) {
			if(FormatHasDepth(image.ci.format)) {
				image_.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			} else if(FormatIsStencilOnly(image.ci.format)) {
				image_.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
			} else {
				image_.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			}

			image_.level = 0u;

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

			image_.level = image_.draw.level;
			image_.aspect = image_.draw.aspect;
		}

		auto getViewType = [&]{
			switch(image.ci.imageType) {
				case VK_IMAGE_TYPE_1D:
					return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
				case VK_IMAGE_TYPE_2D:
					return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				case VK_IMAGE_TYPE_3D:
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
		ivi.subresourceRange.aspectMask = image_.aspect;
		ivi.subresourceRange.baseMipLevel = image_.level;
		ivi.subresourceRange.layerCount = image_.object->ci.arrayLayers;
		ivi.subresourceRange.levelCount = 1u;

		VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &image_.view));
		nameHandle(dev, image_.view, "ResourceGui:image_.view");
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
	ImGui::Text("%s", vk::flagNames(VkImageCreateFlagBits(ci.flags)).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResDesc(draw, image);

	ImGui::Spacing();

	if(image.views.empty()) {
		ImGui::Text("No image views");
	} else if(image.views.size() == 1) {
		ImGui::Text("Image View");
		ImGui::SameLine();
		refButton(*gui_, nonNull(image.views[0]));
	} else if(image.views.size() > 1) {
		ImGui::Text("Image Views:");

		for(auto* view : image.views) {
			ImGui::Bullet();
			refButton(*gui_, nonNull(view));
		}
	}

	// content
	if(image.swapchain) {
		ImGui::Text("Image can't be displayed since it's a swapchain image of");
		ImGui::SameLine();
		refButton(*gui_, *image.swapchain);
	} else if(!image.allowsNearestSampling) {
		ImGui::Text("Image can't be displayed since its format does not support sampling");
	} else if(image.ci.samples != VK_SAMPLE_COUNT_1_BIT) {
		ImGui::Text("Image can't be displayed since it has multiple samples");
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
		draw.usedHandles.push_back(image_.object);

		ImGui::Spacing();
		ImGui::Spacing();

		VkImageSubresourceRange subres {};
		subres.layerCount = image_.object->ci.arrayLayers;
		subres.levelCount = image_.object->ci.mipLevels;
		subres.aspectMask = image_.aspect;

		ReadBuf texelData;
		if(image_.readPixelBuffer.size) {
			// TODO: keep buffer mapped
			// TODO: invalidate and stuff
			void* map;
			VK_CHECK(dev.dispatch.MapMemory(dev.handle, image_.readPixelBuffer.mem,
				0, image_.readPixelBuffer.size, 0, &map));
			texelData = {reinterpret_cast<const std::byte*>(map), image_.readPixelBuffer.size};
		}

		auto format = image_.object->ci.format;
		displayImage(*gui_, image_.draw, image_.object->ci.extent,
			image_.object->ci.imageType, format, subres,
			&image_.readPixelOffset, texelData);

		if(!texelData.empty()) {
			dev.dispatch.UnmapMemory(dev.handle, image_.readPixelBuffer.mem);
		}

		// We always update the descriptor set, not only when we recreate
		// a view, since we can never know about the used draw.
		// Could store whether this is actually needed but not worth it.
		VkDescriptorImageInfo dsii {};
		dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		dsii.imageView = image_.view;
		dsii.sampler = dev.renderData->nearestSampler;
		// dsii.sampler = image.allowsLinearSampling ?
		// 	dev.renderData->linearSampler :
		// 	dev.renderData->nearestSampler;

		VkWriteDescriptorSet write {};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.descriptorCount = 1u;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.dstSet = draw.dsSelected;
		write.pImageInfo = &dsii;

		dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);
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

				{"uint", VK_FORMAT_R32_UINT},
				{"uint2", VK_FORMAT_R32G32_UINT},
				{"uint3", VK_FORMAT_R32G32B32_UINT},
				{"uint4", VK_FORMAT_R32G32B32A32_UINT},
				{"uvec1", VK_FORMAT_R32_UINT},
				{"uvec2", VK_FORMAT_R32G32_UINT},
				{"uvec3", VK_FORMAT_R32G32B32_UINT},
				{"uvec4", VK_FORMAT_R32G32B32A32_UINT},

				{"int", VK_FORMAT_R32_SINT},
				{"int2", VK_FORMAT_R32G32_SINT},
				{"int3", VK_FORMAT_R32G32B32_SINT},
				{"int4", VK_FORMAT_R32G32B32A32_SINT},
				{"ivec1", VK_FORMAT_R32_SINT},
				{"ivec2", VK_FORMAT_R32G32_SINT},
				{"ivec3", VK_FORMAT_R32G32B32_SINT},
				{"ivec4", VK_FORMAT_R32G32B32A32_SINT},
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
				if (start != lt.npos) {
					lt = lt.substr(start);
				}
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

void ResourceGui::drawDesc(Draw&, DescriptorSet& ds) {
	imGuiText("{}", name(ds));
	ImGui::Spacing();

	dlg_assert(ds.state);
	refButtonExpect(*gui_, ds.state->layout.get());
	refButtonExpect(*gui_, ds.pool);

	ImGui::Text("Bindings");
	for(auto b = 0u; b < ds.state->layout->bindings.size(); ++b) {
		auto& layout = ds.state->layout->bindings[b];
		dlg_assert(layout.binding == b);

		auto print = [&](VkDescriptorType type, unsigned b, unsigned e) {
			switch(category(type)) {
				case DescriptorCategory::image: {
					auto& binding = images(*ds.state, b)[e];
					if(needsImageView(type)) {
						refButtonExpect(*gui_, binding.imageView.get());
					}
					if(needsImageLayout(type)) {
						imGuiText("{}", vk::name(binding.layout));
					}
					if(needsSampler(type)) {
						refButtonExpect(*gui_, binding.sampler.get());
					}
					break;
				} case DescriptorCategory::buffer: {
					auto& binding = buffers(*ds.state, b)[e];
					refButtonExpect(*gui_, binding.buffer.get());
					ImGui::SameLine();
					drawOffsetSize(binding);
					break;
				} case DescriptorCategory::bufferView: {
					auto& binding = bufferViews(*ds.state, b)[e];
					refButtonExpect(*gui_, binding.get());
					break;
				} default:
					dlg_warn("Unimplemented descriptor category");
					break;
			}
		};

		auto elemCount = descriptorCount(*ds.state, b);
		if(elemCount > 1) {
			auto label = dlg::format("{}: {}[{}]", b,
				vk::name(layout.descriptorType), elemCount);
			if(ImGui::TreeNode(label.c_str())) {
				for(auto e = 0u; e < elemCount; ++e) {
					ImGui::Bullet();
					imGuiText("{}: ", e);
					ImGui::SameLine();

					print(layout.descriptorType, b, e);
				}
			}
		} else {
			ImGui::Bullet();
			imGuiText("{}, {}: ", b, vk::name(layout.descriptorType));

			ImGui::Indent();
			ImGui::Indent();

			print(layout.descriptorType, b, 0);

			ImGui::Unindent();
			ImGui::Unindent();
		}
	}
}

void ResourceGui::drawDesc(Draw&, DescriptorPool& dsPool) {
	imGuiText("{}", name(dsPool));
	ImGui::Spacing();

	imGuiText("maxSets: {}", dsPool.maxSets);

	ImGui::Text("Sizes");
	for(auto& size : dsPool.poolSizes) {
		imGuiText("{}: {}", vk::name(size.type), size.descriptorCount);
	}

	ImGui::Text("Descriptors");
	for(auto* ds : dsPool.descriptorSets) {
		ImGui::Bullet();
		refButtonExpect(*gui_, ds);
	}
}

void ResourceGui::drawDesc(Draw&, DescriptorSetLayout& dsl) {
	imGuiText("{}", name(dsl));
	ImGui::Spacing();

	ImGui::Text("Bindings");

	for(auto& binding : dsl.bindings) {
		// TODO: immutable samplers
		// TODO: ext_descriptor_indexing flags
		if(binding.descriptorCount > 1) {
			ImGui::BulletText("%s[%d]: {%s}",
				vk::name(binding.descriptorType),
				binding.descriptorCount,
				vk::flagNames(VkShaderStageFlagBits(binding.stageFlags)).c_str());
		} else {
			ImGui::BulletText("%s: {%s}",
				vk::name(binding.descriptorType),
				vk::flagNames(VkShaderStageFlagBits(binding.stageFlags)).c_str());
		}
	}
}

void ResourceGui::drawDesc(Draw&, GraphicsPipeline& pipe) {
	imGuiText("{}", name(pipe));
	ImGui::Spacing();

	auto& dev = gui_->dev();

	// general info
	// text
	ImGui::Columns(2);

	ImGui::Text("Layout");
	ImGui::Text("Render Pass");
	ImGui::Text("Subpass");

	// data
	ImGui::NextColumn();

	refButtonExpect(*gui_, pipe.layout.get());

	// TODO: allow to display RenderPassDesc
	// if(ImGui::Button(name(*pipe.renderPass).c_str())) {
	// 	select(*pipe.renderPass);
	// }

	ImGui::Text("TODO: link to render pass desc");
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
				auto& iv = *entryPoint.input_variables[i];

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
				auto& ov = *entryPoint.output_variables[i];

				if(ImGui::TreeNode(&ov, "%d: %s", ov.location, ov.name)) {
					asColumns2({{
						{"Format", "{}", vk::name(VkFormat(ov.format))},
						{"Storage", "{}", ov.storage_class},
					}});

					ImGui::TreePop();
				}
			}

			if(entryPoint.descriptor_set_count) {
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
			}

			if(stage.stage == VK_SHADER_STAGE_COMPUTE_BIT) {
				ImGui::Text("Workgroup size: %d %d %d",
					entryPoint.local_size.x,
					entryPoint.local_size.y,
					entryPoint.local_size.z);
			}

			/*
			if(ImGui::Button("Open in Vim")) {
				namespace fs = std::filesystem;

				auto fileName = dlg::format("vil.{}.spv", (std::uint64_t) stage.spirv.get());
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

			if(contains(dev.allExts, VK_AMD_SHADER_INFO_EXTENSION_NAME)) {
				if(ImGui::TreeNode("AMD shader info")) {
					VkShaderStatisticsInfoAMD info {};
					auto size = sizeof(info);

					VK_CHECK(dev.dispatch.GetShaderInfoAMD(dev.handle, pipe.handle,
						stage.stage, VK_SHADER_INFO_TYPE_STATISTICS_AMD,
						&size, &info));

					// TODO: info.computeWorkGroupSize?
					asColumns2({{
						{"Available SGPR", "{}", info.numAvailableSgprs},
						{"Available VGPR", "{}", info.numAvailableVgprs},
						{"Physical SGPR", "{}", info.numPhysicalSgprs},
						{"Physical VGPR", "{}", info.numPhysicalVgprs},
						{"Used SGPR", "{}", info.resourceUsage.numUsedSgprs},
						{"Used VGPR", "{}", info.resourceUsage.numUsedVgprs},
						{"Scratch Mem Usage", "{}", info.resourceUsage.scratchMemUsageInBytes},
						{"Scratch Mem Usage", "{}", info.resourceUsage.scratchMemUsageInBytes},
						{"lds Usage", "{}", info.resourceUsage.ldsUsageSizeInBytes},
						{"lds Per Local Workgroup", "{}", info.resourceUsage.ldsSizePerLocalWorkGroup},
					}});

					ImGui::TreePop();
				}
			}

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
	for(auto& dsl : pipeLayout.descriptors) {
		ImGui::Bullet();
		refButtonExpect(*gui_, dsl.get());
	}
}
void ResourceGui::drawDesc(Draw&, CommandPool& cp) {
	imGuiText("{}", name(cp));
	ImGui::Spacing();

	const auto& qprops = cp.dev->queueFamilies[cp.queueFamily].props;
	imGuiText("Queue Family: {} ({})", cp.queueFamily,
		vk::flagNames(VkQueueFlagBits(qprops.queueFlags)));

	for(auto& cb : cp.cbs) {
		refButtonExpect(*gui_, cb);
	}
}

void ResourceGui::drawDesc(Draw&, DeviceMemory& mem) {
	imGuiText("{}", name(mem));
	ImGui::Spacing();

	// info
	ImGui::Columns(2);

	ImGui::Text("Size");
	ImGui::Text("Type Index");

	// data
	ImGui::NextColumn();

	imGuiText("{}", sepfmt(mem.size));
	imGuiText("{}", sepfmt(mem.typeIndex));

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	ImGui::Text("Bound Resources:");

	auto* drawList = ImGui::GetWindowDrawList();

	auto width = ImGui::GetContentRegionAvail().x;
	auto height = 30.f;

	auto start = ImGui::GetCursorScreenPos();
	auto end = start;
	end.x += width;
	end.y += height;

	ImU32 bgCol = IM_COL32(20, 20, 20, 180);
	// ImU32 bgHoverCol = IM_COL32(35, 35, 35, 200);
	ImU32 allocCol = IM_COL32(180, 250, 200, 255);
	ImU32 allocHoverCol = IM_COL32(250, 180, 200, 255);

	drawList->AddRectFilled(start, end, bgCol);
	for(auto& resource : mem.allocations) {
		auto resOff = width * float(resource->allocationOffset) / mem.size;
		auto resSize = width * float(resource->allocationSize) / mem.size;

		auto resPos = start;
		resPos.x += resOff;

		auto rectSize = ImVec2(resSize, height);

		auto col = allocCol;
		auto name = dlg::format("{}", (void*) &resource);

		ImGui::SetCursorScreenPos(resPos);
		ImGui::InvisibleButton(name.c_str(), rectSize);
		if(ImGui::IsItemHovered()) {
			col = allocHoverCol;

			ImGui::BeginTooltip();
			imGuiText("{}", vil::name(*resource));
			imGuiText("Offset: {}", sepfmt(resource->allocationOffset));
			imGuiText("Size: {}", sepfmt(resource->allocationSize));
			ImGui::EndTooltip();
		}
		if(ImGui::IsItemClicked()) {
			select(*resource);
		}

		auto resEnd = ImVec2(resPos.x + rectSize.x, resPos.y + rectSize.y);
		drawList->AddRectFilled(resPos, resEnd, col);
	}
}

void ResourceGui::drawDesc(Draw&, CommandBuffer& cb) {
	ImGui::Text("%s", name(cb).c_str());
	ImGui::Spacing();

	// TODO: more info about cb

	ImGui::Text("Pool: ");
	ImGui::SameLine();
	refButton(*gui_, cb.pool());

	// NOTE: we don't show "invalid" anymore since we don't track
	// that correctly in all cases (mainly descriptors). It's not
	// very important anyways.
	auto stateName = [](auto state) {
		switch(state) {
			case CommandBuffer::State::invalid: /*return "invalid";*/
			case CommandBuffer::State::executable: return "executable";
			case CommandBuffer::State::initial: return "initial";
			case CommandBuffer::State::recording: return "recording";
			default: return "unknonw";
		}
	};

	ImGui::Text("State: %s", stateName(cb.state()));

	// maybe show commands inline (in tree node)
	// and allow via button to switch to cb viewer?
	if(cb.lastRecordLocked()) {
		if(ImGui::Button("View Last Recording")) {
			gui_->cbGui().select(cb.lastRecordPtrLocked(), cb);
			gui_->activateTab(Gui::Tab::commandBuffer);
		}
	} else {
		imGuiText("CommandBuffer was never recorded");
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

	refButtonD(*gui_, view.img);

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
			refButtonExpect(*gui_, fb);
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
	if(fb.imageless) {
		imGuiText("Framebuffer is imageless, has no attachments");
	} else {
		ImGui::Spacing();
		ImGui::Text("Attachments:");

		for(auto* view : fb.attachments) {
			ImGui::Bullet();
			refButtonExpect(*gui_, view);
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
		if(ImGui::TreeNode(&att, "Attachment %d: %s", i, vk::name(att.format))) {
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
		if(ImGui::TreeNode(&subp, "Subpass %d", i)) {
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

	// dependencies
	auto formatSubpass = [](const u32 subpass) {
		if(subpass == VK_SUBPASS_EXTERNAL) {
			return std::string("external");
		}

		return std::to_string(subpass);
	};

	for(auto i = 0u; i < rp.desc->dependencies.size(); ++i) {
		const auto& dep = rp.desc->dependencies[i];
		if(ImGui::TreeNode(&dep, "Dependency %d", i)) {
			asColumns2({{
				{"srcSubpass", formatSubpass(dep.srcSubpass)},
				{"srcAccessMask", vk::flagNames(VkAccessFlagBits(dep.srcAccessMask))},
				{"srcStageMask", vk::flagNames(VkPipelineStageFlagBits(dep.srcStageMask))},
				{"dstSubpass", formatSubpass(dep.dstSubpass)},
				{"dstAccessMask", vk::flagNames(VkAccessFlagBits(dep.dstAccessMask))},
				{"dstStageMask", vk::flagNames(VkPipelineStageFlagBits(dep.dstStageMask))},
				{"dependencyFlags", vk::flagNames(VkDependencyFlagBits(dep.dependencyFlags))},
				{"viewOffset", dep.viewOffset},
			}});

			ImGui::TreePop();
		}
	}

	// TODO: ext data
}

void ResourceGui::drawDesc(Draw&, Event&) {
	// TODO: submission rework/display
}

void ResourceGui::drawDesc(Draw&, Semaphore&) {
	// TODO: submission rework/display
}

void ResourceGui::drawDesc(Draw&, Fence& fence) {
	ImGui::Text("%s", name(fence).c_str());
	ImGui::Spacing();

	// TODO: display associated submission, if any
}
void ResourceGui::drawDesc(Draw&, BufferView& bufView) {
	ImGui::Text("%s", name(bufView).c_str());
	ImGui::Spacing();

	refButtonD(*gui_, bufView.buffer);
	ImGui::SameLine();
	imGuiText("Offset {}, Size {}", bufView.ci.offset, bufView.ci.range);

	imGuiText("{}", vk::name(bufView.ci.format));
}
void ResourceGui::drawDesc(Draw&, QueryPool& pool) {
	ImGui::Text("%s", name(pool).c_str());
	ImGui::Spacing();

	imGuiText("Query type: {}", vk::name(pool.ci.queryType));
	imGuiText("Query count: {}", pool.ci.queryCount);
	imGuiText("Pipeline statistics: {}",
		vk::flagNames(VkQueryPipelineStatisticFlagBits(pool.ci.pipelineStatistics)));
}

void ResourceGui::drawDesc(Draw&, Queue& queue) {
	ImGui::Text("%s", name(queue).c_str());
	ImGui::Spacing();

	const auto& qprops = queue.dev->queueFamilies[queue.family].props;

	imGuiText("Queue Family: {} ({})", queue.family,
		vk::flagNames(VkQueueFlagBits(qprops.queueFlags)));
	imGuiText("Priority: {}", queue.priority);

	imGuiText("Submission Counter: {}", queue.submissionCounter);
}

void ResourceGui::drawDesc(Draw&, Swapchain& swapchain) {
	ImGui::Text("%s", name(swapchain).c_str());
	ImGui::Spacing();

	auto& sci = swapchain.ci;
	asColumns2({{
		{"Format", vk::name(sci.imageFormat)},
		{"Color Space", vk::name(sci.imageColorSpace)},
		{"Width", sci.imageExtent.width},
		{"Height", sci.imageExtent.height},
		{"Present Mode", vk::name(sci.presentMode)},
		{"Transform", vk::name(sci.preTransform)},
		{"Alpha", vk::name(sci.compositeAlpha)},
		{"Image Usage", vk::flagNames(VkImageUsageFlagBits(sci.imageUsage))},
		{"Array Layers", sci.imageArrayLayers},
		{"Min Image Count", sci.minImageCount},
		{"Clipped", sci.clipped},
	}});

	ImGui::Spacing();
	ImGui::Text("Images");

	for(auto& image : swapchain.images) {
		ImGui::Bullet();
		refButtonExpect(*gui_, image);
	}
}

void ResourceGui::drawDesc(Draw& draw, Pipeline& pipe) {
	switch(pipe.type) {
		case VK_PIPELINE_BIND_POINT_GRAPHICS:
			drawDesc(draw, static_cast<GraphicsPipeline&>(pipe));
			return;
		case VK_PIPELINE_BIND_POINT_COMPUTE:
			drawDesc(draw, static_cast<ComputePipeline&>(pipe));
			return;
		default:
			dlg_warn("Unimplemented pipeline bind point");
			return;
	}
}

void ResourceGui::drawDesc(Draw& draw, AccelStruct& accelStruct) {
	(void) draw;
	(void) accelStruct;
	imGuiText("TODO");
}

void ResourceGui::drawDesc(Draw& draw, DescriptorUpdateTemplate& dut) {
	(void) draw;
	(void) dut;
	imGuiText("TODO");
}

void ResourceGui::draw(Draw& draw) {
	auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
	if(!ImGui::BeginTable("Resource viewer", 2, flags, ImGui::GetContentRegionAvail())) {
		return;
	}

	ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, 250.f);
	ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	ImGui::BeginChild("Search settings", {0.f, 0.f}, false);

	// filter by object type
	auto update = firstUpdate_;
	firstUpdate_ = false;

	if(ImGui::Button("Update")) {
		update = true;
	}

	ImGui::SameLine();
	auto filterName = vil::name(filter_);
	// ImGui::SetNextItemWidth(150.f);
	if(ImGui::BeginCombo("", filterName)) {
		for(auto& typeHandler : ObjectTypeHandler::handlers) {
			auto filter = typeHandler->objectType();
			auto name = vil::name(filter);
			if(ImGui::Selectable(name)) {
				filter_ = filter;
				update = true;
			}
		}

		ImGui::EndCombo();
	}

	// text search
	if(imGuiTextInput("Search", search_)) {
		update = true;
	}

	auto& dev = gui_->dev();
	if(update) {
		handles_.clear();
		destroyed_.clear();

		for(auto& typeHandler : ObjectTypeHandler::handlers) {
			if(typeHandler->objectType() == filter_) {
				handles_ = typeHandler->resources(dev, search_);
				break;
			}
		}
	}

	ImGui::Separator();

	// resource list
	ImGui::BeginChild("Resource List", {0.f, 0.f}, false);

	ImGuiListClipper clipper;
	clipper.Begin(int(handles_.size()));

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 4.f));

	while(clipper.Step()) {
		for(auto i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			auto& handle = *handles_[i];
			auto selected = (&handle == handle_);
			auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
				ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;

			ImGui::PushID(&handle);
			std::string label;

			auto disabled = false;
			if(destroyed_.count(&handle)) {
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
				disabled = true;
				label = "<Destroyed>";
			} else {
				label = name(handle);
			}

			if(selected) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			if(ImGui::TreeNodeEx(label.c_str(), flags)) {
				if(ImGui::IsItemClicked()) {
					select(handle);
				}
			}

			if(disabled) {
				ImGui::PopStyleVar();
				ImGui::PopItemFlag();
			}

			ImGui::PopID();

			/*
			auto& handle = *handles_[i];
			auto selected = (&handle == handle_);
			auto flags = 0u;

			ImGui::PushID(&handle);
			std::string label;

			if(destroyed_.count(&handle)) {
				flags = ImGuiSelectableFlags_Disabled;
				label = "<Destroyed>";
			} else {
				label = name(handle);
			}

			ImGui::AlignTextToFramePadding();
			if(ImGui::Selectable(label.c_str(), selected, flags)) {
				select(handle);
			}

			ImGui::PopID();
			*/
		}
	}

	ImGui::PopStyleVar(2);
	ImGui::EndChild(); // Resource List
	ImGui::EndChild(); // left column child

	ImGui::TableNextColumn();

	// resource view
	ImGui::BeginChild("Resource View", {0.f, 0.f}, false);

	if(handle_) {
		ImGui::PushID(handle_);
		drawHandleDesc(draw, *handle_);
		ImGui::PopID();
	}

	ImGui::EndChild();
	ImGui::EndTable();
}

void ResourceGui::drawHandleDesc(Draw& draw, Handle& handle) {
	auto visitor = TemplateResourceVisitor([&](auto& res) {
		this->drawDesc(draw, res);
	});

	for(auto& handler : ObjectTypeHandler::handlers) {
		if(handler->objectType() == handle.objectType) {
			handler->visit(visitor, handle);
		}
	}
}

void ResourceGui::destroyed(const Handle& handle) {
	auto& dev = gui_->dev();
	if(handle_ == &handle) {
		if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
			if(image_.view) {
				dev.dispatch.DestroyImageView(dev.handle, image_.view, nullptr);
			}

			image_ = {};
		}

		handle_ = nullptr;
	}

	if(handle.objectType == filter_) {
		destroyed_.insert(&handle);
	}
}

void ResourceGui::select(Handle& handle) {
	handle_ = &handle;
	dlg_assert(handle.objectType != VK_OBJECT_TYPE_UNKNOWN);
}

void ResourceGui::recordPreRender(Draw& draw) {
	auto& dev = gui_->dev();

	// image waiting logic
	// if we are displaying an image we have to make sure it is not currently
	// being written somewhere else.
	if(handle_ && handle_ == image_.object && image_.view) {
		auto& img = *image_.object;

		// Make sure the image is in the right layout.
		// And we are allowed to read it
		VkImageMemoryBarrier imgb {};
		imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imgb.image = img.handle;
		// TODO: don't just use imag_.aspect, for depthStencil images,
		// we need to set depth & stencil
		imgb.subresourceRange.aspectMask = image_.aspect;
		imgb.subresourceRange.baseMipLevel = image_.level;
		imgb.subresourceRange.levelCount = 1u;
		imgb.subresourceRange.layerCount = image_.object->ci.arrayLayers;
		imgb.oldLayout = img.pendingLayout;
		imgb.newLayout = img.hasTransferSrc ?
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // our rendering
		imgb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		imgb.dstAccessMask = img.hasTransferSrc ?
			VK_ACCESS_TRANSFER_READ_BIT :
			VK_ACCESS_SHADER_READ_BIT;
		imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

		// TODO: transfer queue ownership.
		// We currently just force concurrent mode on image/buffer creation
		// but that might have performance impact.
		// Requires additional submissions to the other queues.
		// We should first check whether the queue is different in first place.
		// if(img.ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
		// }

		auto dstStage = img.hasTransferSrc ?
			VK_PIPELINE_STAGE_TRANSFER_BIT :
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; // our rendering
		dev.dispatch.CmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // wait for everything
			dstStage, 0, 0, nullptr, 0, nullptr, 1, &imgb);

		// image texel copy
		// TODO: should only copy this when the cursor is hovering the image
		if(img.hasTransferSrc) {
			VkBufferImageCopy copy {};
			copy.imageExtent = {1, 1, 1};
			copy.imageSubresource.aspectMask = image_.draw.aspect;
			copy.imageSubresource.baseArrayLayer = image_.draw.layer;
			copy.imageSubresource.mipLevel = image_.draw.level;
			copy.imageSubresource.layerCount = 1u;
			dlg_assert(
				image_.readPixelOffset.x >= 0 &&
				image_.readPixelOffset.y >= 0 &&
				image_.readPixelOffset.z >= 0);
			[[maybe_unused]] auto w = std::max(img.ci.extent.width >> u32(image_.draw.level), 1u);
			[[maybe_unused]] auto h = std::max(img.ci.extent.height >> u32(image_.draw.level), 1u);
			[[maybe_unused]] auto d = std::max(img.ci.extent.depth >> u32(image_.draw.level), 1u);
			dlg_assert(
				u32(image_.readPixelOffset.x) < w &&
				u32(image_.readPixelOffset.y) < h &&
				u32(image_.readPixelOffset.z) < d);

			copy.imageOffset = image_.readPixelOffset;

			auto dstBufUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			image_.readPixelBuffer.ensure(dev, FormatElementSize(img.ci.format), dstBufUsage);

			dev.dispatch.CmdCopyImageToBuffer(draw.cb, img.handle,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_.readPixelBuffer.buf, 1, &copy);

			// Add barrier for our gui read from the image later on
			imgb.oldLayout = imgb.newLayout;
			imgb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imgb.srcAccessMask = imgb.srcAccessMask;
			imgb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			dev.dispatch.CmdPipelineBarrier(draw.cb,
				VK_PIPELINE_STAGE_TRANSFER_BIT, // wait for everything
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &imgb);
		}
	}

	// copy buffer if needed
	if(handle_ && handle_ == buffer_.handle) {
		auto& buf = *buffer_.handle;
		auto offset = 0u; // TODO: allow to set in gui
		auto size = std::min(buf.ci.size - offset, VkDeviceSize(1024 * 16));

		if(size > draw.readback.copy.size) {
			draw.readback.copy.ensure(dev, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
			VK_CHECK(dev.dispatch.MapMemory(dev.handle,
				draw.readback.copy.mem, 0u, VK_WHOLE_SIZE, 0u, &draw.readback.map));
		}

		VkBufferMemoryBarrier bufb {};
		bufb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		bufb.buffer = buf.handle;
		bufb.offset = offset;
		bufb.size = size;
		bufb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		bufb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		dev.dispatch.CmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, nullptr, 1u, &bufb, 0, nullptr);

		VkBufferCopy copy {};
		copy.srcOffset = offset;
		copy.dstOffset = 0u;
		copy.size = size;
		dev.dispatch.CmdCopyBuffer(draw.cb, buf.handle, draw.readback.copy.buf,
			1, &copy);

		bufb.srcAccessMask = bufb.dstAccessMask;
		bufb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

		dev.dispatch.CmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
			0, nullptr, 1u, &bufb, 0, nullptr);

		// The copied data will be received when the draw finishes, Gui::finishedLocked(Draw&)
		// We have to set this data correctly to the currently selected buffer,
		// it will be compared then so that we only retrieve data we are
		// still interested in.
		draw.readback.offset = offset;
		draw.readback.size = size;
		draw.readback.src = buf.handle;

		// make sure this submission properly synchronized with submissions
		// that also use the buffer (especially on other queues).
		draw.usedHandles.push_back(&buf);
	}
}

void ResourceGui::recoredPostRender(Draw& draw) {
	auto& dev = gui_->dev();

	if(handle_ && handle_ == image_.object && image_.view) {
		auto& img = *image_.object;

		// return it to original layout
		VkImageMemoryBarrier imgb {};
		imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imgb.image = img.handle;
		imgb.subresourceRange.aspectMask = image_.aspect;
		imgb.subresourceRange.baseMipLevel = image_.level;
		imgb.subresourceRange.levelCount = 1u;
		imgb.subresourceRange.layerCount = image_.object->ci.arrayLayers;
		imgb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imgb.newLayout = img.pendingLayout;
		imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

		dlg_assert(
			img.pendingLayout != VK_IMAGE_LAYOUT_PREINITIALIZED &&
			img.pendingLayout != VK_IMAGE_LAYOUT_UNDEFINED);
		imgb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		imgb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

		// TODO: transfer queue ownership.
		// We currently just force concurrent mode on image/buffer creation
		// but that might have performance (and correctness) impact.
		// Requires additional submissions to the other queues.
		// We should first check whether the queue is different in first place.
		// if(selImg->ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
		// }

		dev.dispatch.CmdPipelineBarrier(draw.cb,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, // our rendering
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // wait in everything
			0, 0, nullptr, 0, nullptr, 1, &imgb);
	}
}

} // namespace vil
