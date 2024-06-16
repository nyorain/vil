#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif

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
#include <pipe.hpp>
#include <memory.hpp>
#include <threadContext.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <ds.hpp>
#include <cb.hpp>
#include <rp.hpp>
#include <swapchain.hpp>
#include <sync.hpp>
#include <queryPool.hpp>
#include <accelStruct.hpp>
#include <util/util.hpp>
#include <util/buffmt.hpp>
#include <imgui/imgui_internal.h>
#include <vkutil/enumString.hpp>
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

ResourceGui::~ResourceGui() {
	// unref handles
	clearHandles();
}

void ResourceGui::init(Gui& gui) {
	gui_ = &gui;
	buffer_.viewer.init(gui);
	image_.viewer.init(gui);
}

void ResourceGui::drawMemoryResDesc(Draw&, MemoryResource& res) {
	std::lock_guard lock(gui_->dev().mutex);

	if(res.memory.index() == 1) {
		if(ImGui::TreeNode("Sparse memory bindings")) {
			auto& memState = std::get<1>(res.memory);

			auto printOpaqueLabel = false;
			if(!memState.imageBinds.empty()) {
				imGuiText("Image Binds");
				printOpaqueLabel = true;

				dlg_assert(res.memObjectType == VK_OBJECT_TYPE_IMAGE);
				auto& img = static_cast<const Image&>(res);
				auto layered = img.ci.arrayLayers > 1;
				auto hasMips = img.ci.mipLevels > 1;
				auto printAspect = !FormatIsColor(img.ci.format);

				for(auto& imgBind : memState.imageBinds) {
					ImGui::Separator();

#if 0
					// having to hover here is somewhat annoying.
					imGuiText(ICON_FA_INFO " ");
					if(ImGui::IsItemHovered()) {
						ImGui::BeginTooltip();

						if(layered) {
							imGuiText("Layer: {}", imgBind.subres.arrayLayer);
						}

						if(hasMips) {
							imGuiText("Level: {}", imgBind.subres.mipLevel);
						}

						if(printAspect) {
							imGuiText("Aspect: {}",
								vk::nameImageAspectFlags(imgBind.subres.aspectMask));
						}

						imGuiText("offset {} {} {}",
							imgBind.offset.x,
							imgBind.offset.y,
							imgBind.offset.z);
						imGuiText("extent {} {} {}",
							imgBind.size.width,
							imgBind.size.height,
							imgBind.size.depth);

						ImGui::EndTooltip();
					}
#else
					// TODO: also ugly, maybe make this a table?

					if(layered) {
						imGuiText("Layer: {} ", imgBind.subres.arrayLayer);
						ImGui::SameLine();
					}

					if(hasMips) {
						imGuiText("Level: {} ", imgBind.subres.mipLevel);
						ImGui::SameLine();
					}

					if(printAspect) {
						imGuiText("Aspect: {} ",
							vk::nameImageAspectFlags(imgBind.subres.aspectMask));
						ImGui::SameLine();
					}

					imGuiText("Offset [{}, {}, {}] Size [{}, {}, {}] ",
						imgBind.offset.x, imgBind.offset.y, imgBind.offset.z,
						imgBind.size.width, imgBind.size.height, imgBind.size.depth);
#endif

					ImGui::SameLine();

					ImGui::PushID(&imgBind);
					refButtonExpect(*gui_, imgBind.memory);
					ImGui::PopID();

					ImGui::SameLine();
					imGuiText(" (offset {}, size {})",
						sepfmt(imgBind.memOffset),
						sepfmt(imgBind.memSize));
				}
			}

			if(printOpaqueLabel && !memState.opaqueBinds.empty()) {
				imGuiText("Opaque Binds");
			}
			for(auto& opaqueBind : memState.opaqueBinds) {
				imGuiText("Offset {} ", opaqueBind.resourceOffset);
				ImGui::SameLine();

				ImGui::PushID(&opaqueBind);
				refButtonExpect(*gui_, opaqueBind.memory);
				ImGui::PopID();

				ImGui::SameLine();
				imGuiText(" (offset {}, size {})",
					sepfmt(opaqueBind.memOffset),
					sepfmt(opaqueBind.memSize));
			}

			ImGui::TreePop();
		}
	} else {
		auto& memBind = std::get<0>(res.memory);
		if(memBind.memory) {
			ImGui::Text("Bound to memory ");
			ImGui::SameLine();

			refButtonExpect(*gui_, memBind.memory);

			ImGui::SameLine();
			imGuiText(" (offset {}, size {})",
				sepfmt(memBind.memOffset),
				sepfmt(memBind.memSize));
		}
	}
}

void ResourceGui::drawImageContents(Draw& draw, Image& image, bool doSelect) {
	// some other resource was selected in the meantime
	if(image_.object != &image) {
		return;
	}

	IntrusivePtr<Swapchain> swapchain;

	{
		std::lock_guard lock(gui_->dev().mutex);
		swapchain.reset(image.swapchain);
	}

	if(swapchain) {
		ImGui::Text("Image can't be displayed since it's a swapchain image of");
		ImGui::SameLine();
		refButtonExpect(*gui_, swapchain.get());
		return;
	} else if(!image.allowsNearestSampling) {
		ImGui::Text("Image can't be displayed since its format does not support sampling");
		return;
	} else if(image.ci.samples != VK_SAMPLE_COUNT_1_BIT) {
		ImGui::Text("Image can't be displayed since it has multiple samples");
		return;
	} else if(image.ci.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
		ImGui::Text("Transient Image can't be displayed");
		return;
	}

	// NOTE: well, we could still try to display it.
	// But we have to modify our barrier logic a bit.
	// And should probably at least output a warning here that it's in
	// undefined layout and therefore may contain garbage, nothing
	// we can do about that (well, once again not entirely true, we could
	// prevent this invalidation by hooking into state transitions
	// and prevent images from being put into undefined layout; always
	// storing in renderpass and so on. But that's really something
	// for waaaay later, i'm already wondering wth i'm doing with my life
	// writing this).
	constexpr auto displayUndefined = true;
	if(!displayUndefined) {
		std::lock_guard lock(gui_->dev().mutex);
		for(auto& subresLayout : image.pendingLayoutLocked()) {
			if(subresLayout.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
				ImGui::Text("Image can't be displayed since it's in undefined layout, "
					"has undefined content");
				return;
			}
		}
	}

	VkImage imageHandle {};
	constexpr auto layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if(image.memory.index() == 0) {
		FullMemoryBind::State memState {};

		{
			std::lock_guard lock(image.dev->mutex);
			auto& memBind = std::get<0>(image.memory);
			memState = memBind.memState;
			if(memState == FullMemoryBind::State::bound) {
				dlg_assert(image.handle);
				dlg_assert(memBind.memory);
				draw.usedImages.push_back({image_.object, layout});
				imageHandle = image.handle;
			}
		}

		if(memState == FullMemoryBind::State::unbound) {
			ImGui::Text("Can't display contents since image was never bound to memory");
			return;
		} else if(memState == FullMemoryBind::State::memoryDestroyed) {
			ImGui::Text("Can't display image contents since the memory "
				"it was bound to was destroyed");
			return;
		}
	} else if(image.memory.index() == 1) {
		std::lock_guard lock(image.dev->mutex);
		draw.usedImages.push_back({image_.object, layout});
		imageHandle = image.handle;

		auto& memBind = std::get<1>(image.memory);
		for(auto& bind : memBind.imageBinds) {
			if(!bind.memory) {
				imGuiText("Can't display image since it contains invalid "
					"memory bindings (non-opaque), cannot be accessed");
				return;
			}
		}

		for(auto& bind : memBind.opaqueBinds) {
			if(!bind.memory) {
				imGuiText("Can't display image since it contains invalid "
					"memory bindings (opaque), cannot be accessed");
				return;
			}
		}
	}

	if(!imageHandle) {
		ImGui::Text("Can't display contents since image was destroyed");
		return;
	}

	// NOTE: useful for destruction race repro/debugging
	// std::this_thread::sleep_for(std::chrono::milliseconds(30));

	if(doSelect) {
		VkImageSubresourceRange subres {};
		subres.layerCount = image.ci.arrayLayers;
		subres.levelCount = image.ci.mipLevels;
		subres.aspectMask = aspects(image.ci.format);
		auto flags = ImageViewer::preserveSelection | ImageViewer::preserveZoomPan;
		if(image.hasTransferSrc) {
			flags |= ImageViewer::supportsTransferSrc;
		}

		image_.viewer.reset(true);
		image_.viewer.select(imageHandle, image.ci.extent,
			image.ci.imageType, image.ci.format, subres,
			layout, layout, flags);
	}

	ImGui::Spacing();
	ImGui::Spacing();

	image_.viewer.display(draw);
}

void ResourceGui::drawDesc(Draw& draw, Image& image) {
	auto doSelect = (image_.object != &image);
	image_.object = &image;

	// info
	/*
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
	*/

	// resource references
	ImGui::Spacing();
	drawMemoryResDesc(draw, image);

	ImGui::Spacing();

	// make sure the views stay alive while we render this
	std::vector<IntrusivePtr<ImageView>> views;
	{
		std::lock_guard lock(gui_->dev().mutex);
		for(auto* view : image.views) {
			views.emplace_back(view);
		}
	}

	if(views.empty()) {
		ImGui::Text("No image views");
	} else if(views.size() == 1) {
		ImGui::Text("Image View");
		ImGui::SameLine();
		refButtonExpect(*gui_, views[0].get());
	} else if(views.size() > 1) {
		if(ImGui::TreeNode("Image Views")) {
			for(auto& view : views) {
				ImGui::Bullet();
				refButtonExpect(*gui_, view.get());
			}

			ImGui::TreePop();
		}
	}

	drawImageContents(draw, image, doSelect);

	// TODO: display pending layout?
}

void ResourceGui::showBufferViewer(Draw& draw, Buffer& buffer) {
	(void) draw;

	// something else selected in the meantime
	if(buffer_.handle != &buffer) {
		return;
	}

	if(buffer.memory.index() == 0u) {
		// NOTE: this check is racy and we don't insert into usedBuffers yet
		//   since it's only relevant to insert the relevant gui message.
		//   We do the real check (and insert) in the copyBuffer callback.
		FullMemoryBind::State state;
		{
			std::lock_guard lock(buffer.dev->mutex);
			state = std::get<0>(buffer.memory).memState;
		}

		if(state == FullMemoryBind::State::unbound) {
			imGuiText("Can't display buffer content since it isn't bound to memory");
			return;
		} else if(state == FullMemoryBind::State::resourceDestroyed) {
			imGuiText("Can't display buffer content since it was destroyed");
			return;
		} else if(state == FullMemoryBind::State::memoryDestroyed) {
			imGuiText("Can't display buffer content since its memory was destroyed");
			return;
		}
	} else if(buffer.memory.index() == 1u) {
		std::lock_guard lock(buffer.dev->mutex);

		auto& memBind = std::get<1>(buffer.memory);
		dlg_assert(memBind.imageBinds.empty());
		for(auto& bind : memBind.opaqueBinds) {
			if(!bind.memory) {
				imGuiText("Can't display buffer since it contains invalid "
					"memory bindings (opaque), cannot be accessed");
				return;
			}
		}
	}

	gui_->addPostRender([&](Draw& draw) { this->copyBuffer(draw); });
	if(buffer_.lastReadback) {
		auto& readback = buffer_.readbacks[*buffer_.lastReadback];
		dlg_assert(!readback.pending);
		dlg_assert(&buffer == buffer_.handle && readback.src == buffer_.handle->handle);

		ImGui::Separator();
		buffer_.viewer.display(readback.own.data());
	}
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
	showBufferViewer(draw, buffer);
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
	// NOTE: while drawing this, we have the respective pool mutex
	// locked. So it's important we don't lock anything in here, otherwise
	// we might run into a deadlock
	assertOwned(ds.pool->mutex);

	refButtonExpect(*gui_, ds.layout.get());
	refButtonExpect(*gui_, ds.pool);

	ImGui::Text("Bindings");

	// NOTE: with refBindings == false in ds.cpp, we MIGHT get incorrect handles
	//   here. But the chance is small when device has keepAlive maps.
	//   Even when the handles are incorrect, that's only because a view
	//   was destroyed and then a view of the same type constructed
	//   at the same memory address, we validate them inside addCowLocked.
	// TODO: also re-evaluate whether the performance impact of refBindings
	//   is really that large. It really shouldn't be.

	dlg_assert(ds_.state);
	auto state = DescriptorStateRef(*ds_.state);

	for(auto b = 0u; b < ds.layout->bindings.size(); ++b) {
		auto& layout = ds.layout->bindings[b];

		auto print = [&](VkDescriptorType type, unsigned b, unsigned e) {
			switch(category(type)) {
				case DescriptorCategory::image: {
					auto& binding = images(state, b)[e];
					bool append = false;
					if(needsImageView(type)) {
						if(append) {
							ImGui::SameLine();
						}
						refButtonD(*gui_, binding.imageView);
						append = true;
					}
					if(needsImageLayout(type)) {
						if(append) {
							ImGui::SameLine();
						}
						imGuiText("{}", vk::name(binding.layout));
						append = true;
					}
					if(needsSampler(type)) {
						if(append) {
							ImGui::SameLine();
						}
						refButtonD(*gui_, binding.sampler);
						append = true;
					}
					break;
				} case DescriptorCategory::buffer: {
					auto& binding = buffers(state, b)[e];
					refButtonD(*gui_, binding.buffer);
					ImGui::SameLine();
					drawOffsetSize(binding);
					break;
				} case DescriptorCategory::bufferView: {
					auto& binding = bufferViews(state, b)[e];
					refButtonD(*gui_, binding.bufferView);
					break;
				} case DescriptorCategory::accelStruct: {
					auto& binding = accelStructs(state, b)[e];
					refButtonD(*gui_, binding.accelStruct);
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

				ImGui::TreePop();
			}
		} else if(elemCount == 1u) {
			ImGui::Bullet();
			imGuiText("{}, {}: ", b, vk::name(layout.descriptorType));

			ImGui::Indent();
			ImGui::Indent();

			print(layout.descriptorType, b, 0);

			ImGui::Unindent();
			ImGui::Unindent();
		} else if(elemCount == 0u) {
			ImGui::Bullet();
			imGuiText("{}: empty (0 elements)", b);
		}
	}
}

void ResourceGui::drawDesc(Draw&, DescriptorPool& dsPool) {
	imGuiText("maxSets: {}", dsPool.maxSets);

	ImGui::Text("Sizes");
	for(auto& size : dsPool.poolSizes) {
		imGuiText("{}: {}", vk::name(size.type), size.descriptorCount);
	}

	// TODO: show a list of alive descriptorSets
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

	std::vector<CommandBuffer*> cbsCopy;

	{
		std::lock_guard lock(cp.dev->mutex);
		cbsCopy = cp.cbs;
	}

	for(auto& cb : cbsCopy) {
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

	{
		std::lock_guard lock(gui_->dev().mutex);
		for(auto* bind : mem.allocations) {
			auto resOff = width * float(bind->memOffset) / mem.size;
			auto resSize = width * float(bind->memSize) / mem.size;

			auto resPos = start;
			resPos.x += resOff;

			auto rectSize = ImVec2(resSize, height);

			auto col = allocCol;
			auto name = dlg::format("{}", (void*) bind);

			ImGui::SetCursorScreenPos(resPos);
			ImGui::InvisibleButton(name.c_str(), rectSize);

			// TODO: add more details, e.g. for sparse bindings

			auto* resource = bind->resource;
			if(ImGui::IsItemHovered()) {
				col = allocHoverCol;

				ImGui::BeginTooltip();
				imGuiText("{}", vil::name(*resource, resource->memObjectType, true, true));
				imGuiText("Offset: {}", sepfmt(bind->memOffset));
				imGuiText("Size: {}", sepfmt(bind->memSize));
				ImGui::EndTooltip();
			}
			if(ImGui::IsItemClicked()) {
				select(*resource, resource->memObjectType);
			}

			auto resEnd = ImVec2(resPos.x + rectSize.x, resPos.y + rectSize.y);
			drawList->AddRectFilled(resPos, resEnd, col);
		}
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
	auto lastRecord = cb.lastRecordPtr();
	if(lastRecord) {
		if(ImGui::Button("View Last Recording")) {
			gui_->cbGui().select(lastRecord, getCommandBufferPtr(cb));
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

	{
		std::lock_guard lock(gui_->dev().mutex);
		refButtonD(*gui_, view.img);
	}

	ImGui::Text("%s", vk::name(ci.viewType));
	imguiPrintRange(ci.subresourceRange.baseArrayLayer, ci.subresourceRange.layerCount);
	imguiPrintRange(ci.subresourceRange.baseMipLevel, ci.subresourceRange.levelCount);
	ImGui::Text("%s", vk::nameImageAspectFlags(ci.subresourceRange.aspectMask).c_str());
	ImGui::Text("%s", vk::name(ci.format));
	ImGui::Text("%s", vk::nameImageViewCreateFlags(ci.flags).c_str());

	ImGui::Columns();

	// resource references
	ImGui::Spacing();

	// make sure fbs stay alive while we show them
	std::vector<IntrusivePtr<Framebuffer>> fbs;
	{
		std::lock_guard lock(gui_->dev().mutex);
		for(auto* fb : view.fbs) {
			fbs.emplace_back(fb);
		}
	}

	if(!fbs.empty()) {
		ImGui::Text("Framebuffers:");

		for(auto& fb : fbs) {
			ImGui::Bullet();
			refButtonExpect(*gui_, fb.get());
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

		// make sure the views stay alive while we render this
		std::vector<IntrusivePtr<ImageView>> views;
		{
			std::lock_guard lock(gui_->dev().mutex);
			for(auto* view : fb.attachments) {
				views.emplace_back(view);
			}
		}

		for(auto& view : views) {
			ImGui::Bullet();
			refButtonExpect(*gui_, view.get());
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

	// make sure the views stay alive while we render this
	std::vector<IntrusivePtr<Image>> images;
	{
		std::lock_guard lock(gui_->dev().mutex);
		for(auto* img : swapchain.images) {
			images.emplace_back(img);
		}
	}

	for(auto& image : images) {
		ImGui::Bullet();
		refButtonExpect(*gui_, image.get());
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

	AccelStructStatePtr state;

	{
		std::shared_lock lock(gui_->dev().mutex);
		state = accelStruct.lastValid;
	}

	if(!state) {
		imGuiText("Has not been built yet");
		return;
	}

	if(auto* pTris = std::get_if<AccelTriangles>(&state->data); pTris) {
		auto& tris = *pTris;

		auto triCount = 0u;
		for(auto& geom : tris.geometries) {
			triCount += geom.triangles.size();
		}

		imGuiText("{} geometries, {} total tris", tris.geometries.size(), triCount);

		// TODO: better display
		auto& vv = gui_->cbGui().commandViewer().vertexViewer();
		vv.displayTriangles(draw, state->buffer, tris, gui_->dt());

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
	} else if(auto* pBoxes = std::get_if<AccelAABBs>(&state->data); pBoxes) {
		imGuiText("TODO: AABB info");
	} else if(auto* pInis = std::get_if<AccelInstances>(&state->data); pInis) {
		auto& inis = *pInis;
		auto& dev = gui_->dev();

		auto blasResolver = [&](u64 address) -> std::pair<AccelStruct*, AccelStructStatePtr> {
			assertOwnedOrShared(dev.mutex);
			auto* blas = tryAccelStructAtLocked(dev, address);
			dlg_assert(blas);
			if(!blas) {
				return {nullptr, nullptr};
			}

			return {blas, blas->lastValid};
		};

		if(inis.instances.empty()) {
			imGuiText("No instances.");
		} else if(ImGui::TreeNode("Instances")) {
			std::shared_lock lock(dev.mutex);

			for(auto& ini : inis.instances) {
				if(!ini.accelerationStructureReference) {
					imGuiText("null instance");
					continue;
				}

				auto [blas, state] = blasResolver(ini.accelerationStructureReference);
				if(!blas) {
					imGuiText("Error: invalid instance");
					continue;
				}

				ImGui::Separator();
				refButton(*gui_, *blas);

				imGuiText("tableOffset: {}", ini.instanceShaderBindingTableRecordOffset);
				imGuiText("customIndex: {}", ini.instanceCustomIndex);
				imGuiText("mask: {}{}", std::hex, u32(ini.mask));
				imGuiText("flags: {}", vk::nameGeometryInstanceFlagsKHR(ini.flags));

				imGuiText("transform:");
				auto transform = toMat4f(ini.transform);
				for(auto r = 0u; r < 3; ++r) {
					imGuiText("{} {} {} {}",
						transform[r][0],
						transform[r][1],
						transform[r][2],
						transform[r][3]);
				}
			}

			ImGui::TreePop();
		}

		auto blasStateResolver = [&](u64 address) -> AccelStructStatePtr {
			std::shared_lock lock(dev.mutex);
			return blasResolver(address).second;
		};

		// TODO: better display
		auto& vv = gui_->cbGui().commandViewer().vertexViewer();
		vv.displayInstances(draw, inis, gui_->dt(), blasStateResolver);
	}
}

void ResourceGui::drawDesc(Draw& draw, DescriptorUpdateTemplate& dut) {
	(void) draw;
	(void) dut;
	imGuiText("TODO");
}

void ResourceGui::clearHandles() {
	auto decRefCountVisitor = TemplateResourceVisitor([&](auto& res) {
		using HT = std::remove_reference_t<decltype(res)>;
		[[maybe_unused]] constexpr auto noop =
			std::is_same_v<HT, DescriptorSet> ||
			std::is_same_v<HT, Queue>;
		if constexpr(std::is_same_v<HT, Pipeline>) {
			if(res.type == VK_PIPELINE_BIND_POINT_COMPUTE) {
				decRefCount(static_cast<ComputePipeline&>(res));
			} else if(res.type == VK_PIPELINE_BIND_POINT_GRAPHICS) {
				decRefCount(static_cast<GraphicsPipeline&>(res));
			} else if(res.type == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
				decRefCount(static_cast<RayTracingPipeline&>(res));
			} else {
				dlg_error("unreachable");
			}
		} else if constexpr(!noop) {
			decRefCount(res);
		}
	});

	// clear selection
	auto typeHandler = ObjectTypeHandler::handler(filter_);
	for(auto& handle : handles_) {
		typeHandler->visit(decRefCountVisitor, *handle);
	}

	handles_.clear();
	ds_.pools.clear();
	ds_.entries.clear();
}

void ResourceGui::updateResourceList() {
	auto& dev = gui_->dev();

	auto incRefCountVisitor = TemplateResourceVisitor([&](auto& res) {
		using HT = std::remove_reference_t<decltype(res)>;
		[[maybe_unused]] constexpr auto noop =
			std::is_same_v<HT, DescriptorSet> ||
			std::is_same_v<HT, Queue>;
		if constexpr(!noop) {
			incRefCount(res);
		}
	});

	clearHandles();

	// find new handler
	filter_ = newFilter_;

	auto typeHandler = ObjectTypeHandler::handler(filter_);
	std::lock_guard lock(gui_->dev().mutex);

	// find new handles
	auto foundSelected = false;
	if(filter_ == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
		for(auto& dsPool : dev.dsPools.inner) {
			ds_.pools.push_back(dsPool.second);

			auto it = dsPool.second->usedEntries;
			while(it) {
				dlg_assert(it->set);

				auto& entry = ds_.entries.emplace_back();
				entry.pool = dsPool.second.get();
				entry.entry = it;
				entry.id = it->set->id;
				it = it->next;

				if(entry.entry == ds_.selected.entry) {
					foundSelected = true;
				}
			}
		}
	} else {
		handles_ = typeHandler->resources(dev, search_);

		for(auto& handle : handles_) {
			typeHandler->visit(incRefCountVisitor, *handle);
			if(handle == handle_) {
				foundSelected = true;
			}
		}
	}

	// we updated the list and our selection wasn't in there anymore
	if(!foundSelected) {
		clearSelection();
	}
}

void ResourceGui::clearSelection() {
	handle_ = nullptr;
	ds_.selected = {};
	image_.object = {};
	buffer_.handle = {};
}

void ResourceGui::draw(Draw& draw) {
	auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_NoHostExtendY;
	if(!ImGui::BeginTable("Resource viewer", 2, flags, ImGui::GetContentRegionAvail())) {
		return;
	}

	ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthFixed, gui_->uiScale() * 250.f);
	ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 1.f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	ImGui::BeginChild("Search settings", {0.f, 0.f}, false);

	// filter by object type
	auto update = firstUpdate_;
	update |= (filter_ != newFilter_);
	firstUpdate_ = false;

	auto filterName = vil::name(filter_);
	// ImGui::SetNextItemWidth(150.f);
	if(ImGui::BeginCombo(ICON_FA_FILTER, filterName)) {
		for(auto& typeHandler : ObjectTypeHandler::handlers) {
			auto filter = typeHandler->objectType();
			auto name = vil::name(filter);
			if(ImGui::Selectable(name)) {
				newFilter_ = filter;
				update = true;
				clearSelection();
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

	if(update) {
		updateResourceList();
	}

	ImGui::Separator();

	// resource list
	const auto* typeHandler = ObjectTypeHandler::handler(filter_);

	bool isDestroyed {};
	auto isDestroyedVisitor = TemplateResourceVisitor([&](auto& res) {
		using HT = std::remove_reference_t<decltype(res)>;
		if constexpr(std::is_same_v<HT, Queue>) {
			return;
		} else if constexpr(std::is_same_v<HT, DescriptorSet>) {
			dlg_error("unreachable");
			return;
		} else {
			// lock mutex due to access to res.handle
			std::lock_guard lock(gui_->dev().mutex);
			isDestroyed = (res.handle == VK_NULL_HANDLE);
		}
	});

	ImGui::BeginChild("Resource List", {0.f, 0.f}, false);

	ImGuiListClipper clipper;
	if(filter_ == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
		// we can't guarantee the handle stays valid so we
		// never store it, even on selection.
		dlg_assert(handle_ == nullptr);
		clipper.Begin(int(ds_.entries.size()));
	} else {
		clipper.Begin(int(handles_.size()));
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, gui_->uiScale() * ImVec2(2.f, 3.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, gui_->uiScale() * ImVec2(4.f, 4.f));

	while(clipper.Step()) {
		for(auto i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			std::string label;
			bool isSelected;
			bool disable;

			Handle* handle {};
			if(filter_ == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
				auto& entry = ds_.entries[i];
				ImGui::PushID(&entry);

				isSelected = (entry.entry == ds_.selected.entry);

				{
					std::lock_guard lock(entry.pool->mutex);
					isDestroyed = !entry.entry->set ||
						// in this case the dsPool-entry-slot was reused
						(entry.entry->set->id != entry.id);

					if(!isDestroyed) {
						label = name(*entry.entry->set, false);
					}
				}

				disable = isDestroyed;
				if(isDestroyed) {
					label = "<Destroyed>";
					if(isSelected) {
						ds_.selected = {};
						isSelected = false;
					}
				}
			} else {
				handle = handles_[i];
				ImGui::PushID(&handle);

				isSelected = (handle == handle_);
				typeHandler->visit(isDestroyedVisitor, *handle);

				if(isDestroyed) {
					label += "[Destroyed] ";
				}

				// we explicitly allow selecting destroyed handles
				disable = false;
				label += name(*handle, filter_, false, true);
			}

			auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet |
				ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_FramePadding;

			// TODO: for non-ds handles, we could still allow selecting them
			pushDisabled(disable);

			if(isSelected) {
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			if(ImGui::TreeNodeEx(label.c_str(), flags)) {
				if(ImGui::IsItemClicked()) {
					dlg_assert(!disable);
					clearSelection();

					if(filter_ == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
						ds_.selected = ds_.entries[i];
					} else {
						dlg_assert(handle);
						select(*handle, filter_);
					}
				}
			}

			popDisabled(disable);
			ImGui::PopID();
		}
	}

	ImGui::PopStyleVar(2);
	ImGui::EndChild(); // Resource List
	ImGui::EndChild(); // left column child

	ImGui::TableNextColumn();

	// resource view
	ImGui::BeginChild("Resource View", {0.f, 0.f}, false);

	if(filter_ == VK_OBJECT_TYPE_DESCRIPTOR_SET && ds_.selected.entry) {
		ImGui::PushID(ds_.selected.entry);
		drawHandleDesc(draw);
		ImGui::PopID();
	} else if(handle_) {
		// NOTE: automatically unselect on destruction?
		// typeHandler->visit(isDestroyedVisitor, *handle_);
		// if(isDestroyed) {
		// 	handle_ = {};
		// }

		if(handle_) {
			ImGui::PushID(handle_);
			drawHandleDesc(draw);
			ImGui::PopID();
		}
	}

	ImGui::EndChild();
	ImGui::EndTable();
}

void ResourceGui::drawHandleDesc(Draw& draw) {
	auto visitor = TemplateResourceVisitor([&](auto& res) {
		if(editName_) {
			imGuiTextInput("", res.name);
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

	if(filter_ == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
		dlg_assert(ds_.selected.entry);
		dlg_assert(!handle_);

		// update dsState
		// important to reset outside CS
		ds_.state.reset();

		// this separate critical section means that the state can be
		// outdated when we draw it below but that's not a problem
		{
			std::lock_guard devLock(gui_->dev().mutex);
			std::lock_guard poolLock(ds_.selected.pool->mutex);
			auto valid = ds_.selected.entry->set &&
				ds_.selected.entry->set->id == ds_.selected.id;
			if(valid) {
				ds_.state = ds_.selected.entry->set->validateAndCopyLocked();
			}
		}

		// draw
		std::lock_guard lock(ds_.selected.pool->mutex);
		auto valid = ds_.selected.entry->set &&
			ds_.selected.entry->set->id == ds_.selected.id;
		if(valid) {
			drawDesc(draw, *ds_.selected.entry->set);
		} else {
			imGuiText("Was destroyed");
			ds_.selected = {};
		}
	} else {
		for(auto& handler : ObjectTypeHandler::handlers) {
			if(handler->objectType() == filter_) {
				handler->visit(visitor, *handle_);
			}
		}
	}
}

void ResourceGui::select(Handle& handle, VkObjectType type) {
	clearSelection();
	newFilter_ = type;
	editName_ = false;

	dlg_assert(type != VK_OBJECT_TYPE_UNKNOWN);

	if(type == VK_OBJECT_TYPE_DESCRIPTOR_SET) {
		auto& ds = static_cast<DescriptorSet&>(handle);
		// anything else is a race since the DescriptorSet itself could be
		// destroyed any moment
		assertOwned(ds.pool->mutex);
		ds_.selected.entry = ds.setEntry;
		ds_.selected.pool = ds.pool;
		ds_.selected.id = ds.id;
	} else {
		handle_ = &handle;
	}
}

void ResourceGui::copyBuffer(Draw& draw) {
	auto& dev = gui_->dev();

	// might happen if we switched from buffer view to something
	// else in this frame I guess.
	if(!handle_ || handle_ != buffer_.handle) {
		return;
	}

	VkBuffer bufHandle {};

	if(buffer_.handle->memory.index() == 0u) {
		auto& memBind = std::get<0>(buffer_.handle->memory);

		std::lock_guard lock(dev.mutex);
		bool valid = (memBind.memState == FullMemoryBind::State::bound);
		if(!valid) {
			dlg_trace("Detected invalid buffer handle in copyBuffer");
			return;
		}

		dlg_assert(buffer_.handle->handle);
		dlg_assert(memBind.memory);
		draw.usedBuffers.push_back(buffer_.handle);
		bufHandle = buffer_.handle->handle;
	} else if(buffer_.handle->memory.index() == 1u) {
		auto& memBind = std::get<1>(buffer_.handle->memory);

		std::lock_guard lock(dev.mutex);
		if(!buffer_.handle->handle) {
			dlg_trace("Detected invalid buffer handle in copyBuffer");
			return;
		}

		dlg_assert(memBind.imageBinds.empty());
		for(auto& bind : memBind.opaqueBinds) {
			if(!bind.memory) {
				dlg_trace("Detected invalid buffer memory binding in copyBuffer");
				return;
			}
		}

		draw.usedBuffers.push_back(buffer_.handle);
		bufHandle = buffer_.handle->handle;
	}

	// NOTE: useful for destruction race repro/debugging
	// std::this_thread::sleep_for(std::chrono::milliseconds(30));

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
	bufb.buffer = bufHandle;
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
	dev.dispatch.CmdCopyBuffer(draw.cb, bufHandle, readback->own.buf, 1, &copy);

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
	readback->src = bufHandle;
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
}

} // namespace vil
