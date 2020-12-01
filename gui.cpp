#include "gui.hpp"
#include "layer.hpp"
#include "data.hpp"
#include "swapchain.hpp"
#include "util.hpp"
#include "image.hpp"
#include "ds.hpp"
#include "sync.hpp"
#include "shader.hpp"
#include "pipe.hpp"
#include "buffer.hpp"
#include "cb.hpp"
#include "commands.hpp"
#include "rp.hpp"
#include "bytes.hpp"
#include "imguiutil.hpp"

#include "spirv_reflect.h"

#include <set>
#include <map>
#include <fstream>
#include <filesystem>
#include <vkpp/names.hpp>
#include <vkpp/structs.hpp>
#include <imgui/imgui.h>

thread_local ImGuiContext* __LayerImGui;

namespace fuen {

// Gui
void Gui::init(Device& dev) {
	dev_ = &dev;

	this->imgui_ = ImGui::CreateContext();
	ImGui::SetCurrentContext(imgui_);
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetStyle().WindowRounding = 0.f;
	ImGui::GetStyle().WindowBorderSize = 0.f;
}

// ~Gui
Gui::~Gui() {
	if(imgui_) {
		ImGui::DestroyContext(imgui_);
	}

	if(selected_.image.view) {
		dev_->dispatch.vkDestroyImageView(dev_->handle,
			selected_.image.view, nullptr);
	}
}

// Renderer
// Renderer
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
	VkPhysicalDeviceProperties phProps;
	dev.dispatch.vkGetPhysicalDeviceProperties(dev.phdev, &phProps);

	ImGui::Text("Physical device, API version");
	ImGui::Text("Driver version");

	ImGui::NextColumn();

	ImGui::Text("%s %d.%d.%d", phProps.deviceName,
		VK_VERSION_MAJOR(phProps.apiVersion),
		VK_VERSION_MINOR(phProps.apiVersion),
		VK_VERSION_PATCH(phProps.apiVersion));
	ImGui::Text("%d.%d.%d",
		VK_VERSION_MAJOR(phProps.driverVersion),
		VK_VERSION_MINOR(phProps.driverVersion),
		VK_VERSION_PATCH(phProps.driverVersion));

	ImGui::Columns();

	// pretty much just own debug stuff
	ImGui::Separator();

	ImGui::Columns(2);

	ImGui::Text("num submissions");

	ImGui::NextColumn();

	ImGui::Text("%u", u32(dev.pending.size()));

	ImGui::Columns();
}

void Gui::drawMemoryResourceUI(Draw&, MemoryResource& res) {
	if(res.memory) {
		ImGui::Text("Bound to memory ");
		ImGui::SameLine();
		auto label = name(*res.memory);
		if(ImGui::SmallButton(label.c_str())) {
			selected_.handle = res.memory;
		}

		ImGui::SameLine();
		imGuiText(" (offset {}, size {})",
			(unsigned long) res.allocationOffset,
			(unsigned long) res.allocationSize);
	}
}

void Gui::drawResourceUI(Draw& draw, Image& image) {
	ImGui::Text("%s", name(image).c_str());
	ImGui::Spacing();

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

	// info
	auto ci = bit_cast<vk::ImageCreateInfo>(image.ci);
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
	ImGui::Text("%s", vk::name(ci.usage).c_str());
	ImGui::Text("%s", vk::name(ci.tiling));
	ImGui::Text("%s", vk::name(ci.samples));
	ImGui::Text("%s", vk::name(ci.imageType));
	ImGui::Text("%s", vk::name(ci.flags).c_str());

	ImGui::Columns();

	// content
	if(selected_.image.view) {
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::Image((void*) draw.dsSelected, {400, 400});
	}

	// resource references
	ImGui::Spacing();
	drawMemoryResourceUI(draw, image);

	ImGui::Spacing();
	ImGui::Text("Image Views:");

	for(auto* view : image.views) {
		ImGui::Bullet();
		if(ImGui::SmallButton(name(*view).c_str())) {
			select(view);
		}
	}

	// TODO: pending layout?
}

void Gui::drawResourceUI(Draw& draw, Buffer& buffer) {
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
	imGuiText("{}", vk::name(vk::BufferUsageFlags(vk::BufferUsageBits(ci.usage))).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResourceUI(draw, buffer);
}

void Gui::drawResourceUI(Draw&, Sampler& sampler) {
	ImGui::Text("%s", name(sampler).c_str());
	ImGui::Spacing();
	auto ci = bit_cast<vk::SamplerCreateInfo>(sampler.ci);

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

void Gui::drawResourceUI(Draw&, DescriptorSet&) {
	ImGui::Text("TODO");
}

void Gui::drawResourceUI(Draw&, DescriptorPool&) {
	ImGui::Text("TODO");
}

void Gui::drawResourceUI(Draw&, DescriptorSetLayout& dsl) {
	ImGui::Text("%s", name(dsl).c_str());
	ImGui::Spacing();

	ImGui::Text("Bindings");

	for(auto& binding : dsl.bindings) {
		// TODO: immutable samplers
		if(binding.descriptorCount > 1) {
			ImGui::BulletText("%s[%d] in (%s)",
				vk::name(vk::DescriptorType(binding.descriptorType)),
				binding.descriptorCount,
				vk::name(vk::ShaderStageFlags(vk::ShaderStageBits(binding.stageFlags))).c_str());
		} else {
			ImGui::BulletText("%s in (%s)",
				vk::name(vk::DescriptorType(binding.descriptorType)),
				vk::name(vk::ShaderStageFlags(vk::ShaderStageBits(binding.stageFlags))).c_str());
		}
	}
}

void Gui::drawResourceUI(Draw&, GraphicsPipeline& pipe) {
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

	if(ImGui::SmallButton(name(*pipe.layout).c_str())) {
		selected_.handle = pipe.layout;
	}
	if(ImGui::SmallButton(name(*pipe.renderPass).c_str())) {
		selected_.handle = pipe.renderPass;
	}
	ImGui::Text("%d", pipe.subpass);

	ImGui::Columns();
	ImGui::Separator();

	// rasterization
	auto rastInfo = bit_cast<vk::PipelineRasterizationStateCreateInfo>(pipe.rasterizationState);

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

	ImGui::Text("%s", vk::name(rastInfo.cullMode).c_str());
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
		ImGui::Text("%s", vk::name(vk::PrimitiveTopology(pipe.inputAssemblyState.topology)));

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
					vk::name(vk::VertexInputRate(binding.inputRate)), binding.stride);

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
						attrib.location, attrib.offset, vk::name(vk::Format(attrib.format)));
				}

				ImGui::Unindent();
			}

			ImGui::Separator();
		}
	}

	if(!pipe.dynamicState.empty()) {
		ImGui::Text("Dynamic states");

		for(auto& dynState : pipe.dynamicState) {
			ImGui::BulletText("%s", vk::name(vk::DynamicState(dynState)));
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

			ImGui::Text("%s", vk::name(vk::SampleCountBits(pipe.multisampleState.rasterizationSamples)));
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
				ImGui::Text("%s", vk::name(vk::CompareOp(pipe.depthStencilState.depthCompareOp)));

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
		if(ImGui::TreeNode(&stage, "%s", vk::name(vk::ShaderStageBits(stage.stage)))) {
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
						{"Format", "{}", vk::name(vk::Format(iv.format))},
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
						{"Format", "{}", vk::name(vk::Format(ov.format))},
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
							binding.binding, vk::name(vk::DescriptorType(binding.descriptor_type)));
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

			// TODO: used push constants

			ImGui::TreePop();
		}
	}

	// TODO: color blend state
	// TODO: tesselation
}

void Gui::drawResourceUI(Draw&, ComputePipeline&) {
	ImGui::Text("TODO");
}

void Gui::drawResourceUI(Draw&, PipelineLayout& pipeLayout) {
	ImGui::Text("%s", name(pipeLayout).c_str());
	ImGui::Spacing();

	if(!pipeLayout.pushConstants.empty()) {
		ImGui::Text("Push Constants");
		for(auto& pcr : pipeLayout.pushConstants) {
			ImGui::Bullet();
			ImGui::Text("Offset %d, Size %d, in %s", pcr.offset, pcr.size,
				vk::name(vk::ShaderStageFlags(vk::ShaderStageBits(pcr.stageFlags))).c_str());
		}
	}

	ImGui::Text("Descriptor Set Layouts");
	for(auto* ds : pipeLayout.descriptors) {
		ImGui::Bullet();
		if(ImGui::SmallButton(name(*ds).c_str())) {
			select(ds);
		}
	}
}
void Gui::drawResourceUI(Draw&, CommandPool&) {
	ImGui::Text("TODO");
}

void Gui::drawResourceUI(Draw&, DeviceMemory& mem) {
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
		imGuiText("{}: ", resource.offset);

		ImGui::NextColumn();

		if(resource.resource->memoryResourceType == MemoryResource::Type::buffer) {
			Buffer& buffer = static_cast<Buffer&>(*resource.resource);
			auto label = name(buffer);
			ImGui::SmallButton(label.c_str());
		} else if(resource.resource->memoryResourceType == MemoryResource::Type::image) {
			Image& img = static_cast<Image&>(*resource.resource);
			auto label = name(img);
			ImGui::SmallButton(label.c_str());
		}

		ImGui::NextColumn();
		imGuiText("size {}", resource.size);

		ImGui::NextColumn();
	}

	ImGui::Columns();
}

void Gui::drawResourceUI(Draw&, CommandBuffer& cb) {
	// make sure command buffer isn't changed in the meantime
	std::lock_guard lock(cb.mutex);

	ImGui::Text("%s", name(cb).c_str());
	ImGui::Spacing();

	// TODO: more info about cb

	ImGui::Text("Pool: ");
	ImGui::SameLine();
	if(ImGui::SmallButton(name(*cb.pool).c_str())) {
		select(cb.pool);
	}

	// maybe show commands inline (in tree node)
	// and allow via button to switch to cb viewer?
	if(ImGui::Button("View Content")) {
		selected_.cb.cb = &cb;
		activateTab(Tab::commandBuffer);
	}
}

void imguiPrintRange(u32 base, u32 count) {
	if(count > 1) {
		ImGui::Text("[%d, %d]", base, base + count - 1);
	} else {
		ImGui::Text("%d", base);
	}
}

void Gui::drawResourceUI(Draw&, ImageView& view) {
	ImGui::Text("%s", name(view).c_str());
	ImGui::Spacing();

	// info
	ImGui::Columns(2);
	auto ci = bit_cast<vk::ImageViewCreateInfo>(view.ci);

	ImGui::Text("Image");
	ImGui::Text("Type");
	ImGui::Text("Layers");
	ImGui::Text("Levels");
	ImGui::Text("Aspect");
	ImGui::Text("Format");
	ImGui::Text("Flags");

	// data
	ImGui::NextColumn();

	if(ImGui::SmallButton(name(*view.img).c_str())) {
		select(view.img);
	}

	ImGui::Text("%s", vk::name(ci.viewType));
	imguiPrintRange(ci.subresourceRange.baseArrayLayer, ci.subresourceRange.layerCount);
	imguiPrintRange(ci.subresourceRange.baseMipLevel, ci.subresourceRange.levelCount);
	ImGui::Text("%s", vk::name(ci.subresourceRange.aspectMask).c_str());
	ImGui::Text("%s", vk::name(ci.format));
	ImGui::Text("%s", vk::name(ci.flags).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	if(!view.fbs.empty()) {
		ImGui::Text("Framebuffers:");

		for(auto* fb : view.fbs) {
			ImGui::Bullet();
			if(ImGui::SmallButton(name(*fb).c_str())) {
				select(fb);
			}
		}
	}
}

void Gui::drawResourceUI(Draw&, ShaderModule&) {
	ImGui::Text("TODO");
}

void Gui::drawResourceUI(Draw&, Framebuffer& fb) {
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
		if(ImGui::SmallButton(name(*view).c_str())) {
			select(view);
		}
	}
}

void Gui::drawResourceUI(Draw&, RenderPass& rp) {
	ImGui::Text("%s", name(rp).c_str());
	ImGui::Spacing();

	// info
	// attachments
	for(auto i = 0u; i < rp.info.attachments.size(); ++i) {
		auto att = bit_cast<vk::AttachmentDescription>(rp.info.attachments[i]);
		if(ImGui::TreeNode(&rp.info.attachments[i], "Attachment %d: %s", i, vk::name(att.format))) {
			asColumns2({{
				{"Samples", "{}", vk::name(att.samples)},
				{"Initial Layout", "{}", vk::name(att.initialLayout)},
				{"Final Layout", "{}", vk::name(att.finalLayout)},
				{"Flags", "{}", vk::name(att.flags).c_str()},
				{"Load Op", "{}", vk::name(att.loadOp)},
				{"Store Op", "{}", vk::name(att.storeOp)},
				{"Stencil Load Op", "{}", vk::name(att.stencilLoadOp)},
				{"Stencil Store Op", "{}", vk::name(att.stencilStoreOp)},
			}});

			ImGui::TreePop();
		}
	}

	// subpasses
	for(auto i = 0u; i < rp.info.subpasses.size(); ++i) {
		auto subp = bit_cast<vk::SubpassDescription>(rp.info.subpasses[i]);
		if(ImGui::TreeNode(&rp.info.subpasses[i], "Subpass %d", i)) {
			asColumns2({{
				{"Pipeline Bind Point", "{}", vk::name(subp.pipelineBindPoint)},
				{"Flags", "{}", vk::name(subp.flags).c_str()},
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
				ImGui::Text("DepthStencil Attachment: %d, %s", att.attachment, vk::name(att.layout));
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

void Gui::drawCommandBufferInspector(Draw&, CommandBuffer& cb) {
	// make sure command buffer isn't changed in the meantime
	std::lock_guard lock(cb.mutex);

	// Command list
	ImGui::BeginChild("Command list", {400, 0});
	ImGui::PushID(dlg::format("{}:{}", &cb, cb.resetCount).c_str());

	// TODO: add selector ui
	auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
	auto* nsel = displayCommands(cb.commands, selected_.cb.command, flags);
	if(nsel) {
		selected_.cb.resetCount = selected_.cb.cb->resetCount;
		selected_.cb.command = nsel;
	}

	if(selected_.cb.resetCount != selected_.cb.cb->resetCount) {
		selected_.cb.command = nullptr;
	}

	ImGui::PopID();
	ImGui::EndChild();
	ImGui::SameLine();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	if(selected_.cb.command) {
		selected_.cb.command->displayInspector(*this);
		// ImGui::Text("TODO: information about selected command");
	}

	ImGui::EndChild();
}

void Gui::drawResourceSelectorUI(Draw& draw) {
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

	auto filterName = filter_ == 0 ? "all" : vk::name(vk::ObjectType(filter_));
	if(ImGui::BeginCombo("Filter", filterName)) {
		for(auto& filter : filters) {
			auto name = filter == 0 ? "all" : vk::name(vk::ObjectType(filter_));
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
				selected_.handle = entry.second.get();
			}

			ImGui::PopID();
		}
	};

	displayResources(dev_->images);
	displayResources(dev_->imageViews);
	displayResources(dev_->samplers);
	displayResources(dev_->framebuffers);
	displayResources(dev_->renderPasses);
	displayResources(dev_->buffers);
	displayResources(dev_->deviceMemories);
	displayResources(dev_->commandBuffers);
	displayResources(dev_->commandPools);
	displayResources(dev_->dsPools);
	displayResources(dev_->descriptorSets);
	displayResources(dev_->dsLayouts);
	displayResources(dev_->graphicsPipes);
	displayResources(dev_->computePipes);
	displayResources(dev_->pipeLayouts);
	displayResources(dev_->shaderModules);

	ImGui::EndChild();

	// resource view
	ImGui::NextColumn();
	ImGui::BeginChild("Resource View", {0.f, 0.f});

	std::visit(Visitor{
		[&](std::monostate) {},
		[&](auto* selected) {
			ImGui::PushID(selected);
			drawResourceUI(draw, *selected);
			ImGui::PopID();
		}
	}, selected_.handle);

	ImGui::EndChild();
	ImGui::Columns();
}

void Gui::draw(Draw& draw, bool fullscreen) {
	ImGui::NewFrame();

	unsigned flags = 0u;
	if(fullscreen) {
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		flags = ImGuiWindowFlags_NoDecoration;
	} else {
		ImGui::SetNextWindowPos({80, 80}, ImGuiCond_Once);
		ImGui::SetNextWindowSize({800, 500}, ImGuiCond_Once);
		// ImGui::ShowDemoWindow();
		// ImGui::ShowAboutWindow();
		// ImGui::ShowMetricsWindow();
		// auto flags = 0;
	}

	std::shared_lock lock(dev_->mutex);
	auto checkSelectTab = [&](Tab tab) {
		auto flags = 0;
		if(selected_.tab == tab && selected_.tabCounter < 2) {
			flags = ImGuiTabItemFlags_SetSelected;
			++selected_.tabCounter;
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
				drawResourceSelectorUI(draw);
				ImGui::EndTabItem();
			}

			if(selected_.cb.cb) {
				if(ImGui::BeginTabItem("Command Buffer", nullptr, checkSelectTab(Tab::commandBuffer))) {
					drawCommandBufferInspector(draw, *selected_.cb.cb);
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

void Gui::unselect(const Handle& handle) {
	// unselect handle
	auto same = std::visit(Visitor{
		[&](std::monostate) {
			return false;
		}, [&](auto& selected) {
			return selected == &handle;
		}
	}, selected_.handle);

	if(same) {
		selected_.handle = {};
	}

	// special cases
	if(selected_.cb.cb == &handle) {
		selected_.cb.cb = {};
		selected_.cb.command = nullptr;
	}

	if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
		auto& img = static_cast<const Image&>(handle);
		if(img.handle == selected_.image.handle) {
			selected_.image.handle = {};
			if(selected_.image.view) {
				dev_->dispatch.vkDestroyImageView(dev_->handle, selected_.image.view, nullptr);
				selected_.image.view = {};
			}
		}
	}
}

void Gui::activateTab(Tab tab) {
	selected_.tab = tab;
	selected_.tabCounter = 0u;
}

} // namespace fuen
