#include <gui/imageViewer.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <util/util.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/format_utils.h>
#include <vk/enumString.hpp>

namespace vil {

void appendAsInt(std::string& str, double val) {
	str += std::to_string(i64(val));
}

void appendAsDouble(std::string& str, double val) {
	str += std::to_string(val);
}

std::string format(VkFormat format, VkImageAspectFlagBits aspect, Vec4d value) {
	using FmtFunc = void(*)(std::string&, double);
	FmtFunc fmt;
	unsigned channels;

	if(aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
		channels = FormatChannelCount(format);
		fmt = (FormatIsSampledFloat(format)) ? appendAsDouble : appendAsInt;
	} else if(aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
		channels = 1u;
		auto numt = FormatDepthNumericalType(format);
		fmt = (numt == VK_FORMAT_NUMERICAL_TYPE_UINT ||
			   numt == VK_FORMAT_NUMERICAL_TYPE_SINT) ?
			appendAsInt :
			appendAsDouble;
	} else if(aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
		channels = 1u;
		auto numt = FormatStencilNumericalType(format);
		fmt = (numt == VK_FORMAT_NUMERICAL_TYPE_UINT ||
			   numt == VK_FORMAT_NUMERICAL_TYPE_SINT) ?
			appendAsInt :
			appendAsDouble;
	} else {
		dlg_error("Unsupported aspect {}", aspect);
		return "<ERROR>";
	}

	std::string ret;
	for(auto i = 0u; i < channels; ++i) {
		if(i != 0u) {
			ret += ", ";
		}

		fmt(ret, value[i]);
	}

	return ret;
}

void ImageViewer::init(Gui& gui) {
	gui_ = &gui;
}

void ImageViewer::display(Draw& draw) {
	if(!data_) {
		dlg_error("ImageViewer::display without selected image");
		imGuiText("Error: no select image");
		return;
	}

	dlg_assert(src_);

	gui_->addPreRender([&](Draw& draw) { this->recordPreImage(draw.cb); });
	gui_->addPostRender([&](Draw& draw) { this->recordPostImage(draw); });

	ImVec2 pos = ImGui::GetCursorScreenPos();

	auto level = u32(imageDraw_.level);
	auto width = std::max(extent_.width >> level, 1u);
	auto height = std::max(extent_.height >> level, 1u);
	auto depth = std::max(extent_.depth >> level, 1u);

	float regW = ImGui::GetContentRegionAvail().x - 20.f;

	// kinda messy but need this to make avoid flickering for
	// windows that barely need a scrollbar (adding a scrollbar makes
	// the image smaller, causing the content to not need a scrollbar
	// anymore; we will get flickering).
	auto* win = ImGui::GetCurrentWindowRead();
	if(win->ScrollbarY) {
		regW += ImGui::GetStyle().ScrollbarSize;
	}

	float aspect = float(width) / height;
	float regH = regW / aspect;

	auto bgW = regW;
	auto bgH = regH;

	// This is needed for HUGEx1 images (e.g. 1024x1) or 1 dimensional images.
	// We make sure there is at least some area to zoom in.
	bgH = std::max(bgH, 100.f);

	// Cut the image off if too high
	if(regH > regW) {
		bgH = bgW;
		regH = bgH;
		regW = regH * aspect;
	}

	// draw
	// background
	draw_ = &draw;
	auto cbPost = [](const ImDrawList*, const ImDrawCmd* cmd) {
		auto* self = static_cast<ImageViewer*>(cmd->UserCallbackData);
		dlg_assert(self->draw_);
		self->drawBackground(self->draw_->cb);
	};
	ImGui::GetWindowDrawList()->AddCallback(cbPost, this);

	// image
	ImGui::PushClipRect({pos.x, pos.y}, {pos.x + bgW, pos.y + bgH}, true);

	auto clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
	auto clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
	canvasOffset_ = {clipMin.x, clipMin.y};
	canvasSize_ = {clipMax.x - clipMin.x, clipMax.y - clipMin.y};

	auto uv0 = ImVec2(-offset_.x / scale_, -offset_.y / scale_);
	auto endX = bgW / float(regW);
	auto endY = bgH / float(regH);
	auto uv1 = ImVec2((endX - offset_.x) / scale_, (endY - offset_.y) / scale_);
	ImGui::Image((void*) &imageDraw_, {bgW, bgH}, uv0, uv1);
	ImGui::PopClipRect();

	// make sure the view & descriptor we used for drawing stay alive until the
	// draw submission finishes. We do so by pushing an IntrusivePtr to the
	// draw state into the draw, to be reset in onFinish
	auto owner = [drawData = data_](Draw&) mutable { drawData.reset(); };
	draw.onFinish.emplace_back(std::move(owner));

	// logic
	auto& io = ImGui::GetIO();
	ImGui::SetCursorScreenPos(pos);
	ImGui::InvisibleButton("ImageCanvas", {bgW, bgH});
	ImGui::SetItemUsingMouseWheel();
	if(ImGui::IsItemFocused()) {
		// panning
		if(panning_) {
			auto delta = Vec2f{io.MouseDelta.x, io.MouseDelta.y};
			offset_.x += delta.x / regW;
			offset_.y += delta.y / regH;
		}

		if(ImGui::IsItemHovered()) {
			panning_ = io.MouseDown[0];
		}
	}

	if(ImGui::IsItemHovered() && io.MouseWheel != 0.f) {
		// zoom
		auto moff = Vec2f{
			(io.MousePos.x - pos.x) / regW,
			(io.MousePos.y - pos.y) / regH,
		};

		auto sfac = std::pow(1.1, io.MouseWheel);

		offset_.x -= moff.x;
		offset_.y -= moff.y;
		offset_.x *= sfac;
		offset_.y *= sfac;
		offset_.x += moff.x; // * (1 / sfac) ?
		offset_.y += moff.y; // * (1 / sfac) ?

		scale_ *= sfac;
	}

	// Row 0: readback texel value
	if(!copyTexel_) {
		lastReadback_ = {};
	}

	imGuiText("Position: {}, {} | Format {}",
		readTexelOffset_.x, readTexelOffset_.y,
		vk::name(format_));
	ImGui::SameLine();

	bool texelValid = false;
	if(lastReadback_) {
		auto& rb = readbacks_[*lastReadback_];
		dlg_assert(!rb.pending);
		if(rb.valid &&
				rb.texel.x == readTexelOffset_.x &&
				rb.texel.y == readTexelOffset_.y &&
				rb.layer == imageDraw_.layer &&
				rb.level == imageDraw_.level) {
			texelValid = true;
			Vec4d value;

			if(useSamplingCopy) {
				// TODO: precision issues for 32/64 int and doubles.
				auto data = rb.own.data();
				value = Vec4d(read<Vec4f>(data));
			} else {
				auto data = rb.own.data();
				value = read(format_, data);
			}

			imGuiText("| Texel: {}", format(format_, aspect_, value));
		} else {
			// dlg_trace("Rejecting readback: valid: {}, texel: {} {} vs {} {}, "
			// 	"layer {} vs {}, level {} vs {}",
			// 	rb.valid,
			// 	rb.texel.x, readTexelOffset_.x,
			// 	rb.texel.y, readTexelOffset_.y,
			// 	rb.layer, imageDraw_.layer,
			// 	rb.level, imageDraw_.level);
		}
	}

	if(!texelValid) {
		lastReadback_ = {};
		if(copyTexel_) {
			imGuiText("| Texel: Reading...");
		} else {
			imGuiText("| Texel: Can't read Image");
		}
	}

	// Row 1: components
	auto recreateView = false;
	VkFlags depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	if(subresRange_.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		auto numComponents = FormatChannelCount(format_);
		dlg_assert(aspect_ == VK_IMAGE_ASPECT_COLOR_BIT);

		ImGui::CheckboxFlags("R", &imageDraw_.flags, DrawGuiImage::flagMaskR);
		if(numComponents > 1) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("G", &imageDraw_.flags, DrawGuiImage::flagMaskG);
		}

		if(numComponents > 2) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("B", &imageDraw_.flags, DrawGuiImage::flagMaskB);
		}

		if(numComponents > 3) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("A", &imageDraw_.flags, DrawGuiImage::flagMaskA);
		}

		ImGui::SameLine();
		ImGui::CheckboxFlags("Gray", &imageDraw_.flags, DrawGuiImage::flagGrayscale);
	} else if((subresRange_.aspectMask & depthStencil) == depthStencil) {
		dlg_assert(aspect_ == VK_IMAGE_ASPECT_DEPTH_BIT || aspect_ == VK_IMAGE_ASPECT_STENCIL_BIT);

		if(ImGui::RadioButton("Depth", aspect_ == VK_IMAGE_ASPECT_DEPTH_BIT)) {
			aspect_ = VK_IMAGE_ASPECT_DEPTH_BIT;
			recreateView = true;
		}

		ImGui::SameLine();
		if(ImGui::RadioButton("Stencil", aspect_ == VK_IMAGE_ASPECT_STENCIL_BIT)) {
			aspect_ = VK_IMAGE_ASPECT_STENCIL_BIT;
			recreateView = true;
		}
	}

	// Row 2: layer and mip
	if(extent_.depth > 1) {
		// TODO: not very convenient to use for a lot of slices.
		//   make sensitivity absolute, i.e. not dependent on number of slices?
		// TODO: this is weird when the image also has mip levels
		auto maxDepth = depth - 1;
		float layer = imageDraw_.layer * maxDepth;
		ImGui::SliderFloat("slice", &layer, 0, maxDepth);
		imageDraw_.layer = layer / maxDepth;
	} else if(subresRange_.layerCount > 1) {
		int layer = int(imageDraw_.layer);
		ImGui::SliderInt("Layer", &layer, subresRange_.baseArrayLayer,
			subresRange_.baseArrayLayer + subresRange_.layerCount - 1);
		imageDraw_.layer = layer;
	}

	if(subresRange_.levelCount > 1) {
		int mip = int(imageDraw_.level);
		ImGui::SliderInt("Mip", &mip, subresRange_.baseMipLevel,
			subresRange_.baseMipLevel + subresRange_.levelCount - 1);
		imageDraw_.level = mip;
	}

	// Row 3: min/max values
	ImGui::DragFloat("Min", &imageDraw_.minValue, 0.01);
	ImGui::DragFloat("Max", &imageDraw_.maxValue, 0.01);

	// NOTE: could add power/gamma slider here.

	if(recreateView) {
		createData();
	}

	// Process mouse position for texel reading.
	// Make sure we do this *after* we processes all input e.g. for
	// changed mips
	width = std::max(extent_.width >> level, 1u);
	height = std::max(extent_.height >> level, 1u);
	depth = std::max(extent_.depth >> level, 1u);

	auto muv = Vec2f{
		mix(uv0.x, uv1.x, (io.MousePos.x - pos.x) / bgW),
		mix(uv0.y, uv1.y, (io.MousePos.y - pos.y) / bgH),
	};

	readTexelOffset_.x = std::clamp<int>(muv.x * width, 0, width - 1);
	readTexelOffset_.y = std::clamp<int>(muv.y * height, 0, height - 1);
}

void ImageViewer::recordPreImage(VkCommandBuffer cb) {
	auto& dev = gui_->dev();
	DebugLabel cblbl(gui_->dev(), cb, "vil:ImageViewer:pre");

	// prepare image for being drawn
	if(initialImageLayout_ != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		VkImageMemoryBarrier imgb {};
		imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imgb.image = this->src_;
		imgb.subresourceRange = subresRange_;
		imgb.oldLayout = initialImageLayout_;
		imgb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imgb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		imgb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &imgb);
	}
}

void ImageViewer::recordPostImage(Draw& draw) {
	auto cb = draw.cb;
	DebugLabel cblbl(gui_->dev(), cb, "vil:ImageViewer:post");

	auto srcLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	auto srcAccess = VK_ACCESS_SHADER_READ_BIT;
	auto srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	auto needBarrier = finalImageLayout_ != srcLayout;
	if(copyTexel_) {
		constexpr auto useSamplingCopy = true;
		if(useSamplingCopy) {
			doSample(cb, draw, srcLayout);
			srcLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			srcAccess = VK_ACCESS_SHADER_READ_BIT;
			srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			needBarrier = true;
		} else {
			doCopy(cb, draw, srcLayout);
			srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
			srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			needBarrier = true;
		}
	}

	if(needBarrier) {
		auto& dev = gui_->dev();

		VkImageMemoryBarrier imgb {};
		imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imgb.image = this->src_;
		imgb.subresourceRange = subresRange_;
		imgb.oldLayout = srcLayout;
		imgb.newLayout = finalImageLayout_;
		imgb.srcAccessMask = srcAccess;
		imgb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		dev.dispatch.CmdPipelineBarrier(cb,
			srcStage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // defer
			0, 0, nullptr, 0, nullptr, 1, &imgb);
	}
}

void ImageViewer::drawBackground(VkCommandBuffer cb) {
	auto& dev = gui_->dev();
	auto displaySize = ImGui::GetIO().DisplaySize;

	if(canvasSize_.x == 0u || canvasSize_.y == 0u) {
		return; // nothing to do
	}

	VkRect2D scissor {};
	scissor.offset.x = std::max<int>(canvasOffset_.x, 0);
	scissor.offset.y = std::max<int>(canvasOffset_.y, 0);
	scissor.extent.width = std::min<int>(
		canvasSize_.x + canvasOffset_.x - scissor.offset.x,
		displaySize.x - canvasOffset_.x);
	scissor.extent.height = std::min<int>(
		canvasSize_.y + canvasOffset_.y - scissor.offset.y,
		displaySize.y - canvasOffset_.y);
	dev.dispatch.CmdSetScissor(cb, 0, 1, &scissor);

	VkViewport viewport {};
	viewport.width = canvasSize_.x;
	viewport.height = canvasSize_.y;
	viewport.x = canvasOffset_.x;
	viewport.y = canvasOffset_.y;
	viewport.maxDepth = 1.f;
	dev.dispatch.CmdSetViewport(cb, 0, 1, &viewport);

	struct {
		Vec2f qsize;
	} pcData = {
		{20.f / canvasSize_.x, 20.f / canvasSize_.y}
	};

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		gui_->imageBgPipe());
	dev.dispatch.CmdPushConstants(cb, gui_->pipeLayout(),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(pcData), &pcData);
	dev.dispatch.CmdDraw(cb, 4, 1, 0, 0);
}

void ImageViewer::doSample(VkCommandBuffer cb, Draw& draw, VkImageLayout srcLayout) {
	auto& dev = gui_->dev();
	dlg_assert(this->copyTexel_);

	// find free readback or create a new one
	Readback* readback {};
	for(auto [i, r] : enumerate(readbacks_)) {
		if(!r.pending && (!lastReadback_ || i != *lastReadback_)) {
			readback = &r;
			break;
		}
	}

	if(!readback) {
		readback = &readbacks_.emplace_back();

		// TODO: can we always be sure this is enough?
		auto maxBufSize = 1024;
		readback->own.ensure(dev, maxBufSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

		VkDescriptorSetAllocateInfo dsai {};
		dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dsai.descriptorPool = dev.dsPool;
		dsai.descriptorSetCount = 1u;
		dsai.pSetLayouts = &gui_->imgOpDsLayout();

		VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dsai, &readback->opDS));
		nameHandle(dev, readback->opDS, "ImageViewer:readbackOpDs");
	}

	dlg_assert(readback->own.size >= FormatElementSize(format_));

	VkImageMemoryBarrier imgb {};
	imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgb.image = this->src_;
	imgb.subresourceRange = subresRange_;
	imgb.oldLayout = srcLayout;
	imgb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	imgb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // wait for everything
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &imgb);

	// update ds
	VkDescriptorImageInfo imgInfo {};
	imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgInfo.imageView = data_->view;

	VkDescriptorBufferInfo bufInfo {};
	bufInfo.buffer = readback->own.buf;
	bufInfo.offset = 0u;
	bufInfo.range = readback->own.size;

	VkWriteDescriptorSet writes[2] {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].descriptorCount = 1u;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &bufInfo;
	writes[0].dstBinding = 0u;
	writes[0].dstSet = readback->opDS;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].descriptorCount = 1u;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &imgInfo;
	writes[1].dstBinding = 1u;
	writes[1].dstSet = readback->opDS;

	dev.dispatch.UpdateDescriptorSets(dev.handle, 2u, writes, 0u, nullptr);

	// prepare sample copy operation
	auto& pipe = gui_->readTexPipe(imageDraw_.type);
	auto& pipeLayout = gui_->imgOpPipeLayout();

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

	struct {
		Vec3i coords;
		int level;
	} pcr;

	pcr.level = int(imageDraw_.level);
	pcr.coords.x = readTexelOffset_.x;
	if(imgType_ == VK_IMAGE_TYPE_1D) {
		pcr.coords.y = imageDraw_.layer;
		pcr.coords.z = 0; // z irrelevant
	} else {
		pcr.coords.y = readTexelOffset_.y;
		pcr.coords.z = imageDraw_.layer;
	}

	// validate parameters
	// things here shouldn't go wrong (but often did end up to somehow
	// in the past) and assertions should be fixed.
	[[maybe_unused]] auto layerBegin = subresRange_.baseArrayLayer;
	[[maybe_unused]] auto layerEnd = layerBegin + subresRange_.layerCount;
	[[maybe_unused]] auto levelBegin = subresRange_.baseMipLevel;
	[[maybe_unused]] auto levelEnd = levelBegin + subresRange_.levelCount;
	[[maybe_unused]] auto w = std::max(extent_.width >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto h = std::max(extent_.height >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto d = std::max(extent_.height >> u32(imageDraw_.level), 1u);

	dlg_assert_or(pcr.coords.x >= 0, pcr.coords.x = 0);
	dlg_assert_or(pcr.coords.y >= 0, pcr.coords.y = 0);
	dlg_assert_or(u32(pcr.coords.x) < w, pcr.coords.x = w - 1);
	dlg_assert_or(u32(pcr.coords.y) < h, pcr.coords.y = h - 1);
	dlg_assert_or(u32(pcr.coords.z) < d, pcr.coords.z = d - 1);
	dlg_assert(imageDraw_.layer >= layerBegin);
	dlg_assert(imageDraw_.layer < layerEnd);
	dlg_assert_or(pcr.level >= i32(levelBegin), pcr.level = levelBegin);
	dlg_assert_or(pcr.level < i32(levelEnd), pcr.level = levelEnd - 1);

	dev.dispatch.CmdPushConstants(cb, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
		0u, sizeof(pcr), &pcr);
	dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipeLayout, 0, 1, &readback->opDS, 0, nullptr);
	dev.dispatch.CmdDispatch(cb, 1, 1, 1);

	readback->valid = true;
	readback->level = imageDraw_.level;
	readback->layer = imageDraw_.layer;
	readback->texel = readTexelOffset_;
	readback->pending = &draw;

	// register callback to be called when gpu batch finishes execution
	auto cbFinish = [this](Draw& draw) {
		auto found = false;
		for(auto [i, readback] : enumerate(readbacks_)) {
			if(readback.pending == &draw) {
				dlg_assert(!found);
				found = true;
				readback.pending = nullptr;
				lastReadback_ = i;
				// break;
			}
		}
	};

	draw.onFinish.push_back(cbFinish);
}

void ImageViewer::doCopy(VkCommandBuffer cb, Draw& draw, VkImageLayout srcLayout) {
	auto& dev = gui_->dev();
	dlg_assert(this->copyTexel_);
	dlg_assertm(FormatTexelBlockExtent(format_).width == 1u,
		"Block formats not supported for copying");

	// find free readback or create a new one
	Readback* readback {};
	for(auto [i, r] : enumerate(readbacks_)) {
		if(!r.pending && (!lastReadback_ || i != *lastReadback_)) {
			readback = &r;
			break;
		}
	}

	if(!readback) {
		readback = &readbacks_.emplace_back();

		// TODO: can we always be sure this is enough?
		auto maxBufSize = 1024;
		readback->own.ensure(dev, maxBufSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	}

	dlg_assert(readback->own.size >= FormatElementSize(format_));

	// TODO: fix handling of depth-stencil images. We probably have to pass
	// multiple copy regions (one for each aspect, see the docs of
	// VkBufferImageCopy). But then also fix the aspects in the barriers.

	VkImageMemoryBarrier imgb {};
	imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgb.image = this->src_;
	imgb.subresourceRange = subresRange_;
	imgb.oldLayout = srcLayout;
	imgb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	imgb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	imgb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	imgb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	// TODO: transfer queue ownership.
	// We currently just force concurrent mode on image/buffer creation
	// but that might have performance impact.
	// Requires additional submissions to the other queues.
	// We should first check whether the queue is different in first place.
	// if(img.ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
	// }

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // wait for everything
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &imgb);

	// copy
	VkBufferImageCopy copy {};
	copy.imageExtent = {1, 1, 1};
	copy.imageSubresource.aspectMask = aspect_;
	copy.imageSubresource.mipLevel = imageDraw_.level;
	copy.imageSubresource.layerCount = 1u;

	copy.imageOffset.x = readTexelOffset_.x;
	copy.imageOffset.y = readTexelOffset_.y;
	if(imgType_ == VK_IMAGE_TYPE_3D) {
		copy.imageOffset.z = imageDraw_.layer * extent_.depth;
	} else {
		copy.imageSubresource.baseArrayLayer = imageDraw_.layer;
	}

	// validate parameters
	// things here shouldn't go wrong (but often did end up to somehow
	// in the past) and assertions should be fixed.
	[[maybe_unused]] auto layerBegin = subresRange_.baseArrayLayer;
	[[maybe_unused]] auto layerEnd = layerBegin + subresRange_.layerCount;
	[[maybe_unused]] auto levelBegin = subresRange_.baseMipLevel;
	[[maybe_unused]] auto levelEnd = levelBegin + subresRange_.levelCount;
	[[maybe_unused]] auto w = std::max(extent_.width >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto h = std::max(extent_.height >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto d = std::max(extent_.height >> u32(imageDraw_.level), 1u);

	dlg_assert_or(copy.imageOffset.x >= 0, copy.imageOffset.x = 0);
	dlg_assert_or(copy.imageOffset.y >= 0, copy.imageOffset.y = 0);
	dlg_assert_or(u32(copy.imageOffset.x) < w, copy.imageOffset.x = w - 1);
	dlg_assert_or(u32(copy.imageOffset.y) < h, copy.imageOffset.y = h - 1);
	dlg_assert_or(u32(copy.imageOffset.z) < d, copy.imageOffset.z = d - 1);
	dlg_assert_or(copy.imageSubresource.baseArrayLayer >= layerBegin,
		copy.imageSubresource.baseArrayLayer = layerBegin);
	dlg_assert_or(copy.imageSubresource.baseArrayLayer < layerEnd,
		copy.imageSubresource.baseArrayLayer = layerEnd - 1);
	dlg_assert_or(copy.imageSubresource.mipLevel >= levelBegin,
		copy.imageSubresource.mipLevel = levelBegin);
	dlg_assert_or(copy.imageSubresource.mipLevel < levelEnd,
		copy.imageSubresource.mipLevel = levelEnd - 1);

	dev.dispatch.CmdCopyImageToBuffer(cb, src_,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->own.buf, 1, &copy);

	readback->valid = true;
	readback->level = imageDraw_.level;
	readback->layer = imageDraw_.layer;
	readback->texel = readTexelOffset_;
	readback->pending = &draw;

	// register callback to be called when gpu batch finishes execution
	auto cbFinish = [this](Draw& draw) {
		auto found = false;
		for(auto [i, readback] : enumerate(readbacks_)) {
			if(readback.pending == &draw) {
				dlg_assert(!found);
				found = true;
				readback.pending = nullptr;
				lastReadback_ = i;
				// break;
			}
		}
	};

	draw.onFinish.push_back(cbFinish);
}

VkImageAspectFlagBits defaultAspect(VkImageAspectFlags flags) {
	if(flags & VK_IMAGE_ASPECT_DEPTH_BIT) {
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	} else if(flags & VK_IMAGE_ASPECT_STENCIL_BIT) {
		return VK_IMAGE_ASPECT_STENCIL_BIT;
	} else if(flags & VK_IMAGE_ASPECT_COLOR_BIT) {
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}

	dlg_error("unreachable");
	return {};
}

void ImageViewer::select(VkImage src, VkExtent3D extent, VkImageType imgType,
		VkFormat format, const VkImageSubresourceRange& subresRange,
		VkImageLayout initialLayout, VkImageLayout finalLayout,
		u32 /*Flags*/ flags) {
	src_ = src;
	extent_ = extent;
	imgType_ = imgType;
	format_ = format;
	subresRange_ = subresRange;
	initialImageLayout_ = initialLayout;
	finalImageLayout_ = finalLayout;
	copyTexel_ = useSamplingCopy || (flags & supportsTransferSrc);

	draw_ = {};

	// auto supportedAspects = aspects(format);
	auto supportedAspects = subresRange.aspectMask;
	if(!(supportedAspects & aspect_) || !(flags & preserveSelection)) {
		auto newAspect = defaultAspect(supportedAspects);
		if(newAspect != aspect_ || !(flags & preserveSelection)) {
			if(newAspect == VK_IMAGE_ASPECT_COLOR_BIT) {
				auto numComponents = FormatChannelCount(format_);
				imageDraw_.flags = DrawGuiImage::flagMaskR;
				if(numComponents >= 2) {
					imageDraw_.flags |= DrawGuiImage::flagMaskG;
				}

				if(numComponents >= 3) {
					imageDraw_.flags |= DrawGuiImage::flagMaskB;
				}
			} else {
				// depth or stencil
				imageDraw_.flags = DrawGuiImage::flagMaskR | DrawGuiImage::flagGrayscale;
			}
		}

		aspect_ = newAspect;
	}

	if(!(flags & preserveZoomPan)) {
		offset_ = {};
		scale_ = {1.f};
	}

	if(!(flags & preserveSelection)) {
		imageDraw_.minValue = 0.f;
		imageDraw_.maxValue = 1.f;
		imageDraw_.level = subresRange.baseMipLevel;
		if(imgType == VK_IMAGE_TYPE_3D) {
			imageDraw_.layer = subresRange.baseArrayLayer;
		} else {
			imageDraw_.layer = 0u;
		}
	} else {
		// imageDraw_.level = std::clamp(
		// 	subresRange.baseMipLevel,
		// 	subresRange.baseMipLevel + subresRange.levelCount,
		// 	imageDraw_.level);
		auto lvlBegin = subresRange.baseMipLevel;
		auto lvlEnd = lvlBegin + subresRange.levelCount;
		if(imageDraw_.level < lvlBegin || imageDraw_.level >= lvlEnd) {
			imageDraw_.level = lvlBegin;
		}

		auto layerBegin = subresRange.baseArrayLayer;
		auto layerEnd = layerBegin + subresRange.baseArrayLayer;
		if(imgType == VK_IMAGE_TYPE_3D) {
			layerBegin = 0u;
			layerEnd = 1u; // extent.depth;
		}

		if(imageDraw_.layer < layerBegin || imageDraw_.layer >= layerEnd) {
			imageDraw_.layer = layerBegin;
		}
	}

	if(!(flags & preserveReadbacks)) {
		for(auto& readback : readbacks_) {
			readback.valid = false;
		}
	}

	imageDraw_.type = ShaderImageType::parseType(imgType, format, aspect_);
	dlg_assertm(imageDraw_.type != ShaderImageType::count,
		"imgType {}, format {}", vk::name(imgType_), vk::name(format_));

	createData();
}

void ImageViewer::reset() {
	unselect();
	imageDraw_ = {};
	offset_ = {};
	scale_ = 1.f;
}

void ImageViewer::unselect() {
	src_ = {};
	draw_ = {};
}

void ImageViewer::createData() {
	data_.reset(new DrawData());
	data_->gui = gui_;

	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = src_;
	ivi.viewType = imageViewForImageType(imgType_);
	ivi.format = format_;
	ivi.subresourceRange = subresRange_;
	ivi.subresourceRange.aspectMask = aspect_;

	auto& dev = gui_->dev();
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &data_->view));
	nameHandle(dev, data_->view, "ImageViewer:imageView");

	// create descriptor
	VkDescriptorSetAllocateInfo dsai {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = dev.dsPool;
	dsai.descriptorSetCount = 1u;
	dsai.pSetLayouts = &gui_->dsLayout();
	VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dsai, &data_->ds));
	nameHandle(dev, data_->ds, "ImageViewer:ds");

	VkDescriptorImageInfo dsii {};
	dsii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	dsii.imageView = data_->view;
	// TODO: use better sampler, with linear interpolation where possible.
	// But only when image is too large for canvas, otherwise use nearest via
	// min/magFilter
	dsii.sampler = dev.nearestSampler;
	// dsii.sampler = image.allowsLinearSampling ?
	// 	dev.renderData->linearSampler :
	// 	dev.renderData->nearestSampler;

	VkWriteDescriptorSet write {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1u;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.dstSet = data_->ds;
	write.pImageInfo = &dsii;

	dev.dispatch.UpdateDescriptorSets(dev.handle, 1, &write, 0, nullptr);

	imageDraw_.ds = data_->ds;
}

ImageViewer::DrawData::~DrawData() {
	if(!gui) {
		return;
	}

	auto& dev = gui->dev();
	dev.dispatch.FreeDescriptorSets(dev.handle, dev.dsPool, 1, &ds);
	dev.dispatch.DestroyImageView(dev.handle, view, nullptr);
}

} // namespace vil
