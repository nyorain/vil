#include <gui/resources.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/command.hpp>
#include <gui/cb.hpp>
#include <gui/vertexViewer.hpp>
#include <gui/fontAwesome.hpp>
#include <command/commands.hpp>
#include <device.hpp>
#include <queue.hpp>
#include <threadContext.hpp>
#include <handles.hpp>
#include <accelStruct.hpp>
#include <util/util.hpp>
#include <util/buffmt.hpp>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
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
		if(counter == 3u) {
			counter = 0u;
			ret.insert(0, 1, '\'');
		}

		auto rest = (size % (10 * poten)) / poten;
		size -= rest * poten;
		ret.insert(0, 1, nums[rest]);

		poten *= 10;
		++counter;
	}

	return ret;
}

void ResourceGui::init(Gui& gui) {
	gui_ = &gui;
	buffer_.viewer.init(gui);
	image_.viewer.init(gui);
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
	auto doSelect = (image_.object != &image);
	image_.object = &image;

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
	ImGui::Text("%s", vk::nameImageUsageFlags(ci.usage).c_str());
	ImGui::Text("%s", vk::name(ci.tiling));
	ImGui::Text("%s", vk::name(ci.samples));
	ImGui::Text("%s", vk::name(ci.imageType));
	ImGui::Text("%s", vk::nameImageCreateFlags(ci.flags).c_str());

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
		refButtonExpect(*gui_, image.views[0]);
	} else if(image.views.size() > 1) {
		ImGui::Text("Image Views:");

		for(auto* view : image.views) {
			ImGui::Bullet();
			refButtonExpect(*gui_, view);
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
	} else if(image.ci.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
		ImGui::Text("Transient Image can't be displayed");
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
		if(doSelect) {
			VkImageSubresourceRange subres {};
			subres.layerCount = image.ci.arrayLayers;
			subres.levelCount = image.ci.mipLevels;
			subres.aspectMask = aspects(image.ci.format);
			auto flags = ImageViewer::preserveSelection | ImageViewer::preserveZoomPan;
			if(image.hasTransferSrc) {
				flags |= ImageViewer::supportsTransferSrc;
			}
			image_.viewer.select(image.handle, image.ci.extent,
				image.ci.imageType, image.ci.format, subres,
				image.pendingLayout, image.pendingLayout, flags);
		}

		draw.usedHandles.push_back(image_.object);

		ImGui::Spacing();
		ImGui::Spacing();

		image_.viewer.display(draw);
	}

	// TODO: display pending layout?
}

void ResourceGui::drawDesc(Draw& draw, Buffer& buffer) {
	if(buffer_.handle != &buffer) {
		// TODO: remember used layouts per-buffer?
		// But would be nice to have mechanism for that across multiple starts
		buffer_.lastReadback = {};
		buffer_.offset = {};
		buffer_.size = {};
		buffer_.handle = &buffer;
	}

	// info
	ImGui::Columns(2);

	ImGui::SetColumnWidth(0, 100);

	ImGui::Text("Size");
	ImGui::Text("Usage");

	ImGui::NextColumn();

	auto& ci = buffer.ci;
	imGuiText("{}", ci.size);
	imGuiText("{}", vk::nameBufferUsageFlags(ci.usage).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();
	drawMemoryResDesc(draw, buffer);

	// content
	// we are using a child window to avoid column glitches
	gui_->addPostRender([&](Draw& draw) { this->copyBuffer(draw); });
	if(buffer_.lastReadback) {
		auto& readback = buffer_.readbacks[*buffer_.lastReadback];
		dlg_assert(!readback.pending);
		if(readback.src == buffer_.handle->handle) {
			ImGui::Separator();
			buffer_.viewer.display(readback.own.data());
		}
	}
}

void ResourceGui::drawDesc(Draw&, Sampler& sampler) {
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
	refButtonExpect(*gui_, ds.layout.get());
	refButtonExpect(*gui_, ds.pool);

	ImGui::Text("Bindings");

	// TODO: call addCow, then retrieve the bindings. This only
	//   works if refBindings in ds.cpp is set though. Otherwise
	//   the handles stored in the bindings might have been destroyed,
	//   we don't track/notify that. Since activating refBindings
	//   has a huge runtime impact, allow to enable it via meson option
	/*
	std::lock_guard lock(ds.mutex);
	for(auto b = 0u; b < ds.layout->bindings.size(); ++b) {
		auto& layout = ds.layout->bindings[b];
		dlg_assert(layout.binding == b);

		auto print = [&](VkDescriptorType type, unsigned b, unsigned e) {
			switch(category(type)) {
				case DescriptorCategory::image: {
					auto& binding = images(ds, b)[e];
					if(needsImageView(type)) {
						refButtonExpect(*gui_, *binding.imageView.get());
					}
					if(needsImageLayout(type)) {
						imGuiText("{}", vk::name(binding.layout));
					}
					if(needsSampler(type)) {
						refButtonExpect(*gui_, binding.sampler.get());
					}
					break;
				} case DescriptorCategory::buffer: {
					auto& binding = buffers(ds, b)[e];
					refButtonExpect(*gui_, binding.buffer.get());
					ImGui::SameLine();
					drawOffsetSize(binding);
					break;
				} case DescriptorCategory::bufferView: {
					auto& binding = bufferViews(ds, b)[e];
					refButtonExpect(*gui_, binding.get());
					break;
				} default:
					dlg_warn("Unimplemented descriptor category");
					break;
			}
		};

		auto elemCount = descriptorCount(ds, b);
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
	*/
}

void ResourceGui::drawDesc(Draw&, DescriptorPool& dsPool) {
	imGuiText("maxSets: {}", dsPool.maxSets);

	ImGui::Text("Sizes");
	for(auto& size : dsPool.poolSizes) {
		imGuiText("{}: {}", vk::name(size.type), size.descriptorCount);
	}

	// NOTE: maintaining this was a huge pain in the ass.
	// If somebody really needs it, we could implement it though.
	// ImGui::Text("Descriptors");
	// std::lock_guard lock(dsPool.mutex);
	// for(auto* ds : dsPool.descriptorSets) {
	// 	ImGui::Bullet();
	// 	refButtonExpect(*gui_, ds);
	// }
}

void ResourceGui::drawDesc(Draw&, DescriptorSetLayout& dsl) {
	ImGui::Text("Bindings");

	for(auto& binding : dsl.bindings) {
		// TODO: immutable samplers
		// TODO: ext_descriptor_indexing flags
		if(binding.descriptorCount > 1) {
			ImGui::BulletText("%s[%d]: {%s}",
				vk::name(binding.descriptorType),
				binding.descriptorCount,
				vk::nameShaderStageFlags(binding.stageFlags).c_str());
		} else {
			ImGui::BulletText("%s: {%s}",
				vk::name(binding.descriptorType),
				vk::nameShaderStageFlags(binding.stageFlags).c_str());
		}
	}
}

void ResourceGui::drawDesc(Draw&, GraphicsPipeline& pipe) {
	// references: layout & renderPass
	refButtonExpect(*gui_, pipe.layout.get());

	refButtonExpect(*gui_, pipe.renderPass.get());
	ImGui::SameLine();
	ImGui::Text("Subpass %d", pipe.subpass);

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

	ImGui::Text("%s", vk::nameCullModeFlags(rastInfo.cullMode).c_str());
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
			ImGui::Text("TODO");

			drawShaderInfo(pipe.handle, stage.stage);

			ImGui::TreePop();
		}
	}

	// TODO: color blend state
	// TODO: tesselation
}

void ResourceGui::drawShaderInfo(VkPipeline pipe, VkShaderStageFlagBits stage) {
	auto& dev = gui_->dev();
	if(contains(dev.allExts, VK_AMD_SHADER_INFO_EXTENSION_NAME)) {
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		if(ImGui::TreeNode("AMD shader info")) {
			VkShaderStatisticsInfoAMD info {};
			auto size = sizeof(info);

			VK_CHECK(dev.dispatch.GetShaderInfoAMD(dev.handle, pipe,
				stage, VK_SHADER_INFO_TYPE_STATISTICS_AMD,
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
				{"LDS Usage", "{}", info.resourceUsage.ldsUsageSizeInBytes},
				{"LDs Per Local Workgroup", "{}", info.resourceUsage.ldsSizePerLocalWorkGroup},
			}});

			ImGui::TreePop();
		}
	}
}

void ResourceGui::drawDesc(Draw&, ComputePipeline& pipe) {
	ImGui::Text("TODO");

	drawShaderInfo(pipe.handle, VK_SHADER_STAGE_COMPUTE_BIT);
}

void ResourceGui::drawDesc(Draw&, PipelineLayout& pipeLayout) {
	if(!pipeLayout.pushConstants.empty()) {
		ImGui::Text("Push Constants");
		for(auto& pcr : pipeLayout.pushConstants) {
			ImGui::Bullet();
			ImGui::Text("Offset %d, Size %d, in %s", pcr.offset, pcr.size,
				vk::nameShaderStageFlags(pcr.stageFlags).c_str());
		}
	}

	ImGui::Text("Descriptor Set Layouts");
	for(auto& dsl : pipeLayout.descriptors) {
		ImGui::Bullet();
		refButtonExpect(*gui_, dsl.get());
	}
}
void ResourceGui::drawDesc(Draw&, CommandPool& cp) {
	const auto& qprops = cp.dev->queueFamilies[cp.queueFamily].props;
	imGuiText("Queue Family: {} ({})", cp.queueFamily,
		vk::nameQueueFlags(qprops.queueFlags));

	for(auto& cb : cp.cbs) {
		refButtonExpect(*gui_, cb);
	}
}

void ResourceGui::drawDesc(Draw&, DeviceMemory& mem) {
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
	ImU32 allocCol = IM_COL32(130, 220, 150, 255);
	ImU32 allocHoverCol = IM_COL32(250, 150, 180, 255);

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
			gui_->cbGui().select(cb.lastRecordPtrLocked(), getCommandBufferPtr(cb));
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
	ImGui::Text("%s", vk::nameImageAspectFlags(ci.subresourceRange.aspectMask).c_str());
	ImGui::Text("%s", vk::name(ci.format));
	ImGui::Text("%s", vk::nameImageViewCreateFlags(ci.flags).c_str());

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
	asColumns2({{
		{"Width", "{}", fb.width},
		{"Height", "{}", fb.height},
		{"Layers", "{}", fb.layers},
	}});

	refButtonExpect(*gui_, fb.rp.get());

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
	// info
	auto& desc = rp.desc;

	// attachments
	for(auto i = 0u; i < desc.attachments.size(); ++i) {
		const auto& att = desc.attachments[i];
		if(ImGui::TreeNode(&att, "Attachment %d: %s", i, vk::name(att.format))) {
			asColumns2({{
				{"Samples", "{}", vk::name(att.samples)},
				{"Initial Layout", "{}", vk::name(att.initialLayout)},
				{"Final Layout", "{}", vk::name(att.finalLayout)},
				{"Flags", "{}", vk::nameAttachmentDescriptionFlags(att.flags)},
				{"Load Op", "{}", vk::name(att.loadOp)},
				{"Store Op", "{}", vk::name(att.storeOp)},
				{"Stencil Load Op", "{}", vk::name(att.stencilLoadOp)},
				{"Stencil Store Op", "{}", vk::name(att.stencilStoreOp)},
			}});

			ImGui::TreePop();
		}
	}

	// subpasses
	for(auto i = 0u; i < desc.subpasses.size(); ++i) {
		const auto& subp = desc.subpasses[i];
		if(ImGui::TreeNode(&subp, "Subpass %d", i)) {
			asColumns2({{
				{"Pipeline Bind Point", "{}", vk::name(subp.pipelineBindPoint)},
				{"Flags", "{}", vk::nameSubpassDescriptionFlags(subp.flags)},
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

	for(auto i = 0u; i < desc.dependencies.size(); ++i) {
		const auto& dep = desc.dependencies[i];
		if(ImGui::TreeNode(&dep, "Dependency %d", i)) {
			asColumns2({{
				{"srcSubpass", formatSubpass(dep.srcSubpass)},
				{"srcAccessMask", vk::nameAccessFlags(dep.srcAccessMask)},
				{"srcStageMask", vk::namePipelineStageFlags(dep.srcStageMask)},
				{"dstSubpass", formatSubpass(dep.dstSubpass)},
				{"dstAccessMask", vk::nameAccessFlags(dep.dstAccessMask)},
				{"dstStageMask", vk::namePipelineStageFlags(dep.dstStageMask)},
				{"dependencyFlags", vk::nameDependencyFlags(dep.dependencyFlags)},
				{"viewOffset", dep.viewOffset},
			}});

			ImGui::TreePop();
		}
	}

	// TODO: ext data
}

void ResourceGui::drawDesc(Draw&, Event& event) {
	imGuiText("TODO");
	(void) event;
}

void ResourceGui::drawDesc(Draw&, Semaphore& semaphore) {
	imGuiText("Type: {}", vk::name(semaphore.type));
	if(semaphore.type == VK_SEMAPHORE_TYPE_TIMELINE) {
		auto& dev = gui_->dev();
		u64 val;
		dev.dispatch.GetSemaphoreCounterValue(dev.handle, semaphore.handle, &val);
		imGuiText("Value: {}", val);
	}
}

void ResourceGui::drawDesc(Draw&, Fence& fence) {
	imGuiText("TODO");
	(void) fence;

	// TODO: display associated submission, if any
}
void ResourceGui::drawDesc(Draw&, BufferView& bufView) {
	refButtonD(*gui_, bufView.buffer);
	ImGui::SameLine();
	imGuiText("Offset {}, Size {}", bufView.ci.offset, bufView.ci.range);

	imGuiText("{}", vk::name(bufView.ci.format));
}
void ResourceGui::drawDesc(Draw&, QueryPool& pool) {
	imGuiText("Query type: {}", vk::name(pool.ci.queryType));
	imGuiText("Query count: {}", pool.ci.queryCount);
	imGuiText("Pipeline statistics: {}",
		vk::nameQueryPipelineStatisticFlags(pool.ci.pipelineStatistics));
}

void ResourceGui::drawDesc(Draw&, Queue& queue) {
	const auto& qprops = queue.dev->queueFamilies[queue.family].props;

	imGuiText("Queue Family: {} ({})", queue.family,
		vk::nameQueueFlags(qprops.queueFlags));
	imGuiText("Priority: {}", queue.priority);

	imGuiText("Submission Counter: {}", queue.submissionCounter);
}

void ResourceGui::drawDesc(Draw&, Swapchain& swapchain) {
	auto& sci = swapchain.ci;
	asColumns2({{
		{"Format", vk::name(sci.imageFormat)},
		{"Color Space", vk::name(sci.imageColorSpace)},
		{"Width", sci.imageExtent.width},
		{"Height", sci.imageExtent.height},
		{"Present Mode", vk::name(sci.presentMode)},
		{"Transform", vk::name(sci.preTransform)},
		{"Alpha", vk::name(sci.compositeAlpha)},
		{"Image Usage", vk::nameImageUsageFlags(sci.imageUsage)},
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
	refButtonExpect(*gui_, accelStruct.buf);
	ImGui::SameLine();
	imGuiText("Offset {}, Size {}", accelStruct.offset, accelStruct.size);

	imGuiText("type: {}", vk::name(accelStruct.type));
	imGuiText("effective type: {}", vk::name(accelStruct.effectiveType));
	imGuiText("geometry type: {}", vk::name(accelStruct.geometryType));

	if(accelStruct.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
		auto& tris = std::get<AccelTriangles>(accelStruct.data);

		auto triCount = 0u;
		for(auto& geom : tris.geometries) {
			triCount += geom.triangles.size();
		}

		imGuiText("{} geometries, {} total tris", tris.geometries.size(), triCount);

		// TODO: better display
		auto& vv = gui_->cbGui().commandViewer().vertexViewer();
		vv.displayTriangles(draw, tris, gui_->dt());

		auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
			ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;
		for(auto [i, geom] : enumerate(tris.geometries)) {
			auto lbl = dlg::format("Geometry {}", i);
			if(!ImGui::TreeNodeEx(lbl.c_str(), flags)) {
				continue;
			}

			// TODO; store/show indices for better debugging?
			auto nd = std::min<unsigned>(100u, geom.triangles.size());
			for(auto tri : geom.triangles.subspan(0, nd)) {
				ImGui::Bullet();
				ImGui::SameLine();
				imGuiText("{}", tri.a);

				ImGui::Bullet();
				ImGui::SameLine();
				imGuiText("{}", tri.b);

				ImGui::Bullet();
				ImGui::SameLine();
				imGuiText("{}", tri.c);

				ImGui::Separator();
			}
		}
	} else if(accelStruct.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
		imGuiText("TODO: AABB info");
	} else if(accelStruct.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
		auto& inis = std::get<AccelInstances>(accelStruct.data);

		for(auto& ini : inis.instances) {
			ImGui::Separator();
			refButtonExpect(*gui_, ini.accelStruct);

			imGuiText("tableOffset: {}", ini.bindingTableOffset);
			imGuiText("customIndex: {}", ini.customIndex);
			imGuiText("mask: {}{}", std::hex, u32(ini.mask));
			imGuiText("flags: {}", vk::nameGeometryInstanceFlagsKHR(ini.flags));

			imGuiText("transform:");
			for(auto r = 0u; r < 3; ++r) {
				imGuiText("{} {} {} {}",
					ini.transform[r][0],
					ini.transform[r][1],
					ini.transform[r][2],
					ini.transform[r][3]);
			}
		}

		if(inis.instances.empty()) {
			imGuiText("No instances.");
		}

		imGuiText("TODO: visualize instances");
	}
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

	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_REDO)) {
		update = true;
	}

	// text search
	if(imGuiTextInput(ICON_FA_SEARCH, search_)) {
		update = true;
	}

	auto& dev = gui_->dev();
	if(update) {
		// lock access to destroyed_
		std::lock_guard lock(gui_->dev().mutex);
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
			auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
				ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;

			ImGui::PushID(&handle);

			{
				auto selected = (&handle == handle_);

				// lock access to destroyed_
				std::lock_guard lock(gui_->dev().mutex);
				auto disabled = (destroyed_.count(&handle) > 0);
				std::string label = disabled ? "<Destroyed>" : name(handle, false);

				pushDisabled(disabled);

				if(selected) {
					flags |= ImGuiTreeNodeFlags_Selected;
				}

				if(ImGui::TreeNodeEx(label.c_str(), flags)) {
					if(ImGui::IsItemClicked()) {
						dlg_assert(!disabled);
						select(handle);
					}
				}

				popDisabled(disabled);
			}

			ImGui::PopID();
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
		if(editName_) {
			imGuiTextInput("", handle.name);
			if(ImGui::IsItemDeactivated()) {
				editName_ = false;
			}

			// TODO: forward new debug name to further layers?
			// not sure if that's expected behavior
		} else {
			imGuiText("{}", name(res));
			if(ImGui::IsItemClicked()) {
				editName_ = true;
			}
		}

		ImGui::Spacing();

		this->drawDesc(draw, res);
	});

	for(auto& handler : ObjectTypeHandler::handlers) {
		if(handler->objectType() == handle.objectType) {
			handler->visit(visitor, handle);
		}
	}
}

void ResourceGui::destroyed(const Handle& handle) {
	if(handle_ == &handle) {
		if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
			image_.object = {};
			image_.viewer.unselect();
		}

		handle_ = nullptr;
	}

	// TODO: the DescriptorSet list is broken since we don't
	// get notifications for them.
	if(handle.objectType == filter_) {
		destroyed_.insert(&handle);
	}
}

void ResourceGui::select(Handle& handle) {
	handle_ = &handle;
	editName_ = false;
	dlg_assert(handle.objectType != VK_OBJECT_TYPE_UNKNOWN);
}

void ResourceGui::copyBuffer(Draw& draw) {
	auto& dev = gui_->dev();

	// might happen if we switched from buffer view to something
	// else in this frame I guess.
	if(!handle_ || handle_ != buffer_.handle) {
		return;
	}

	auto& buf = *buffer_.handle;
	auto offset = 0u; // TODO: allow to set in gui
	auto maxCopySize = VkDeviceSize(1 * 1024 * 1024);
	auto size = std::min(buf.ci.size - offset, maxCopySize);

	// find free readback or create a new one
	BufReadback* readback {};
	for(auto [i, r] : enumerate(buffer_.readbacks)) {
		if(!r.pending && (!buffer_.lastReadback || i != *buffer_.lastReadback)) {
			readback = &r;
			break;
		}
	}

	if(!readback) {
		readback = &buffer_.readbacks.emplace_back();
	}

	readback->own.ensure(dev, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

	VkBufferMemoryBarrier bufb {};
	bufb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bufb.buffer = buf.handle;
	bufb.offset = offset;
	bufb.size = size;
	bufb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	bufb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	bufb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(draw.cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, nullptr, 1u, &bufb, 0, nullptr);

	VkBufferCopy copy {};
	copy.srcOffset = offset;
	copy.dstOffset = 0u;
	copy.size = size;
	dev.dispatch.CmdCopyBuffer(draw.cb, buf.handle, readback->own.buf, 1, &copy);

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
	readback->offset = offset;
	readback->size = size;
	readback->src = buf.handle;
	readback->pending = &draw;

	auto cb = [this](Draw& draw, bool success){
		auto found = false;
		for(auto [i, readback] : enumerate(buffer_.readbacks)) {
			if(readback.pending == &draw) {
				dlg_assert(!found);
				found = true;
				readback.pending = nullptr;

				if(success) {
					buffer_.lastReadback = i;
				}
			}
		}

		dlg_assert(found);
	};
	draw.onFinish.push_back(cb);

	// make sure this submission properly synchronized with submissions
	// that also use the buffer (especially on other queues).
	draw.usedHandles.push_back(&buf);
}

} // namespace vil
