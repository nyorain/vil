#include <gui/resources.hpp>
#include <gui/gui.hpp>
#include <handles.hpp>
#include <util.hpp>
#include <imguiutil.hpp>
#include <spirv_reflect.h>
#include <vulkan/vk_enum_string_helper.h>
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

	/*
	if(selected_.image.handle != image.handle) {
		selected_.image.handle = image.handle;

		if(selected_.image.view) {
			dev_->dispatch.vkDestroyImageView(dev_->handle, selected_.image.view, nullptr);
			selected_.image.view = {};
		}

		if(!image.swapchain) {
			selected_.image.aspectMask = isDepthFormat(vk::Format(image.ci.format)) ?
				VK_IMAGE_ASPECT_DEPTH_BIT :
				VK_IMAGE_ASPECT_COLOR_BIT;

			// TODO: fix for non-2d images.
			// TODO: upload sampler here instead of in pipeline layout.
			//   Might have to use nearest sampler instead of lienar for
			//   certain formats
			VkImageViewCreateInfo ivi = vk::ImageViewCreateInfo();
			ivi.image = image.handle;
			ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			ivi.format = image.ci.format;
			ivi.subresourceRange.aspectMask = selected_.image.aspectMask;
			ivi.subresourceRange.layerCount = 1u;
			ivi.subresourceRange.levelCount = 1u;

			auto res = dev_->dispatch.vkCreateImageView(dev_->handle, &ivi, nullptr, &selected_.image.view);
			dlg_assert(res == VK_SUCCESS);

			VkDescriptorImageInfo dsii;
			dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			dsii.imageView = selected_.image.view;

			VkWriteDescriptorSet write = vk::WriteDescriptorSet();
			write.descriptorCount = 1u;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.dstSet = draw.dsSelected;
			write.pImageInfo = &dsii;

			dev_->dispatch.vkUpdateDescriptorSets(dev_->handle, 1, &write, 0, nullptr);
		} else {
			dlg_debug("not creating view due to swapchain image");
		}
	}
	*/

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
	ImGui::Text("%s", string_VkFormat(ci.format));
	ImGui::Text("%s", string_VkImageUsageFlags(ci.usage).c_str());
	ImGui::Text("%s", string_VkImageTiling(ci.tiling));
	ImGui::Text("%s", string_VkSampleCountFlagBits(ci.samples));
	ImGui::Text("%s", string_VkImageType(ci.imageType));
	ImGui::Text("%s", string_VkImageCreateFlags(ci.flags).c_str());

	ImGui::Columns();

	// content
	/*
	if(selected_.image.view) {
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::Image((void*) draw.dsSelected, {400, 400});
	}
	*/

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

	// TODO: pending layout?
}

void ResourceGui::drawDesc(Draw& draw, Buffer& buffer) {
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
	imGuiText("{}", string_VkBufferUsageFlags(ci.usage).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResDesc(draw, buffer);
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

	ImGui::Text("%s", string_VkFilter(ci.minFilter));
	ImGui::Text("%s", string_VkFilter(ci.magFilter));
	ImGui::Text("%s", string_VkSamplerMipmapMode(ci.mipmapMode));
	ImGui::Text("%s", string_VkSamplerAddressMode(ci.addressModeU));
	ImGui::Text("%s", string_VkSamplerAddressMode(ci.addressModeV));
	ImGui::Text("%s", string_VkSamplerAddressMode(ci.addressModeW));
	ImGui::Text("%s", string_VkBorderColor(ci.borderColor));
	ImGui::Text("%d", ci.unnormalizedCoordinates);
	ImGui::Text("%f", ci.minLod);
	ImGui::Text("%f", ci.maxLod);

	if(ci.anisotropyEnable) {
		ImGui::Text("%f", ci.maxAnisotropy);
	}

	if(ci.compareEnable) {
		ImGui::Text("%s", string_VkCompareOp(ci.compareOp));
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
				string_VkDescriptorType(binding.descriptorType),
				binding.descriptorCount,
				string_VkShaderStageFlags(binding.stageFlags).c_str());
		} else {
			ImGui::BulletText("%s in (%s)",
				string_VkDescriptorType(binding.descriptorType),
				string_VkShaderStageFlags(binding.stageFlags).c_str());
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

	ImGui::Text("%s", string_VkCullModeFlags(rastInfo.cullMode).c_str());
	ImGui::Text("%s", string_VkPolygonMode(rastInfo.polygonMode));
	ImGui::Text("%s", string_VkFrontFace(rastInfo.frontFace));

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
		ImGui::Text("%s", string_VkPrimitiveTopology(pipe.inputAssemblyState.topology));

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
					string_VkVertexInputRate(binding.inputRate), binding.stride);

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
						attrib.location, attrib.offset, string_VkFormat(attrib.format));
				}

				ImGui::Unindent();
			}

			ImGui::Separator();
		}
	}

	if(!pipe.dynamicState.empty()) {
		ImGui::Text("Dynamic states");

		for(auto& dynState : pipe.dynamicState) {
			ImGui::BulletText("%s", string_VkDynamicState(dynState));
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

			ImGui::Text("%s", string_VkSampleCountFlagBits(pipe.multisampleState.rasterizationSamples));
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
				ImGui::Text("%s", string_VkCompareOp(pipe.depthStencilState.depthCompareOp));

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
		if(ImGui::TreeNode(&stage, "%s", string_VkShaderStageFlagBits(stage.stage))) {
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
						{"Format", "{}", string_VkFormat(VkFormat(iv.format))},
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
						{"Format", "{}", string_VkFormat(VkFormat(ov.format))},
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
							string_VkDescriptorType(VkDescriptorType(binding.descriptor_type)));
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
				string_VkShaderStageFlags(pcr.stageFlags).c_str());
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

	ImGui::Text("%s", string_VkImageViewType(ci.viewType));
	imguiPrintRange(ci.subresourceRange.baseArrayLayer, ci.subresourceRange.layerCount);
	imguiPrintRange(ci.subresourceRange.baseMipLevel, ci.subresourceRange.levelCount);
	ImGui::Text("%s", string_VkImageAspectFlags(ci.subresourceRange.aspectMask).c_str());
	ImGui::Text("%s", string_VkFormat(ci.format));
	ImGui::Text("%s", string_VkImageViewCreateFlags(ci.flags).c_str());

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
		if(ImGui::TreeNode(&rp.desc->attachments[i], "Attachment %d: %s", i, string_VkFormat(att.format))) {
			asColumns2({{
				{"Samples", "{}", string_VkSampleCountFlagBits(att.samples)},
				{"Initial Layout", "{}", string_VkImageLayout(att.initialLayout)},
				{"Final Layout", "{}", string_VkImageLayout(att.finalLayout)},
				{"Flags", "{}", string_VkAttachmentDescriptionFlags(att.flags).c_str()},
				{"Load Op", "{}", string_VkAttachmentLoadOp(att.loadOp)},
				{"Store Op", "{}", string_VkAttachmentStoreOp(att.storeOp)},
				{"Stencil Load Op", "{}", string_VkAttachmentLoadOp(att.stencilLoadOp)},
				{"Stencil Store Op", "{}", string_VkAttachmentStoreOp(att.stencilStoreOp)},
			}});

			ImGui::TreePop();
		}
	}

	// subpasses
	for(auto i = 0u; i < rp.desc->subpasses.size(); ++i) {
		const auto& subp = rp.desc->subpasses[i];
		if(ImGui::TreeNode(&rp.desc->subpasses[i], "Subpass %d", i)) {
			asColumns2({{
				{"Pipeline Bind Point", "{}", string_VkPipelineBindPoint(subp.pipelineBindPoint)},
				{"Flags", "{}", string_VkSubpassDescriptionFlags(subp.flags).c_str()},
			}});

			ImGui::Separator();
			if(subp.colorAttachmentCount) {
				ImGui::Text("Color Attachments:");
				for(auto c = 0u; c < subp.colorAttachmentCount; ++c) {
					auto& att = subp.pColorAttachments[c];
					ImGui::BulletText("%d, %s", att.attachment, string_VkImageLayout(att.layout));
				}
			}

			if(subp.inputAttachmentCount) {
				ImGui::Text("Input Attachments:");
				for(auto c = 0u; c < subp.inputAttachmentCount; ++c) {
					auto& att = subp.pInputAttachments[c];
					ImGui::BulletText("%d, %s", att.attachment, string_VkImageLayout(att.layout));
				}
			}

			if(subp.pDepthStencilAttachment) {
				auto& att = *subp.pDepthStencilAttachment;
				ImGui::Text("DepthStencil Attachment: %d, %s", att.attachment,
					string_VkImageLayout(att.layout));
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

	auto filterName = filter_ == 0 ? "all" : string_VkObjectType(VkObjectType(filter_));
	if(ImGui::BeginCombo("Filter", filterName)) {
		for(auto& filter : filters) {
			auto name = filter == 0 ? "all" : string_VkObjectType(VkObjectType(filter_));
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
				image_.view = {};
				image_.subres = {};
			}
		}
	}
}

} // namespace fuen
