#include <gui/imageViewer.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <util/util.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/format_utils.h>
#include <vk/enumString.hpp>

namespace vil {

void ImageViewer::init(Gui& gui) {
	gui_ = &gui;
}

void ImageViewer::display(Draw& draw) {
	gui_->addPreRender([&](Draw& draw) { this->recordPreImage(draw.cb); });
	gui_->addPostRender([&](Draw& draw) { this->recordPostImage(draw); });

	ImVec2 pos = ImGui::GetCursorScreenPos();

	auto level = u32(imageDraw_.level);
	auto width = std::max(extent.width >> level, 1u);
	auto height = std::max(extent.height >> level, 1u);
	auto depth = std::max(extent.depth >> level, 1u);

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
	offset_ = {clipMin.x, clipMin.y};
	size_ = {clipMax.x - clipMin.x, clipMax.y - clipMin.y};

	auto uv0 = ImVec2(-this->offset.x / scale, -this->offset.y / scale);
	auto endX = bgW / float(regW);
	auto endY = bgH / float(regH);
	auto uv1 = ImVec2((endX - this->offset.x) / scale, (endY - this->offset.y) / scale);
	ImGui::Image((void*) &imageDraw_, {bgW, bgH}, uv0, uv1);

	ImGui::PopClipRect();

	// logic
	auto& io = ImGui::GetIO();
	ImGui::SetCursorScreenPos(pos);
	ImGui::InvisibleButton("ImageCanvas", {bgW, bgH});
	ImGui::SetItemUsingMouseWheel();
	if(ImGui::IsItemFocused()) {
		// panning
		if(panning_) {
			auto delta = Vec2f{io.MouseDelta.x, io.MouseDelta.y};
			offset.x += delta.x / regW;
			offset.y += delta.y / regH;
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

		auto sfac = std::pow(1.05, io.MouseWheel);

		offset.x -= moff.x;
		offset.y -= moff.y;
		offset.x *= sfac;
		offset.y *= sfac;
		offset.x += moff.x;
		offset.y += moff.y;

		scale *= sfac;
	}

	// process mouse position for texel reading
	auto muv = Vec2f{
		mix(uv0.x, uv1.x, (io.MousePos.x - pos.x) / bgW),
		mix(uv0.y, uv1.y, (io.MousePos.y - pos.y) / bgH),
	};

	readTexelOffset_.x = std::clamp<int>(muv.x * width, 0, width - 1);
	readTexelOffset_.y = std::clamp<int>(muv.y * height, 0, height - 1);
	if(!copyTexel) {
		lastReadback_ = {};
	}

	// Row 0: readback texel value
	imGuiText("Position: {}, {} | Format {}",
		readTexelOffset_.x, readTexelOffset_.y,
		vk::name(format));
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
			auto data = rb.own.data();
			imGuiText("| Texel: {}", read(format, data));
		}
	}

	if(!texelValid) {
		lastReadback_ = {};
		if(copyTexel) {
			imGuiText("| Texel: Reading...");
		} else {
			imGuiText("| Texel: Can't read Image");
		}
	}

	// Row 1: components
	if(subresRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		auto numComponents = FormatChannelCount(format);
		imageDraw_.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

		ImGui::CheckboxFlags("R", &imageDraw_.flags, DrawGuiImage::flagMaskR);
		if(numComponents > 1) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("G", &imageDraw_.flags, DrawGuiImage::flagMaskG);
		} else {
			imageDraw_.flags &= ~(DrawGuiImage::flagMaskG);
		}

		if(numComponents > 2) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("B", &imageDraw_.flags, DrawGuiImage::flagMaskB);
		} else {
			imageDraw_.flags &= ~(DrawGuiImage::flagMaskB);
		}

		if(numComponents > 3) {
			ImGui::SameLine();
			ImGui::CheckboxFlags("A", &imageDraw_.flags, DrawGuiImage::flagMaskA);
		} else {
			imageDraw_.flags &= ~(DrawGuiImage::flagMaskA);
		}

		ImGui::SameLine();
		ImGui::CheckboxFlags("Gray", &imageDraw_.flags, DrawGuiImage::flagGrayscale);
	} else {
		VkFlags depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		imageDraw_.flags = DrawGuiImage::flagMaskR | DrawGuiImage::flagGrayscale;

		// init
		if(imageDraw_.aspect != VK_IMAGE_ASPECT_DEPTH_BIT && imageDraw_.aspect != VK_IMAGE_ASPECT_STENCIL_BIT) {
			imageDraw_.aspect = (subresRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ?
				VK_IMAGE_ASPECT_DEPTH_BIT :
				VK_IMAGE_ASPECT_STENCIL_BIT;
		}

		if((subresRange.aspectMask & depthStencil) == depthStencil) {
			if(ImGui::RadioButton("Depth", imageDraw_.aspect == VK_IMAGE_ASPECT_DEPTH_BIT)) {
				imageDraw_.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			}

			ImGui::SameLine();
			if(ImGui::RadioButton("Stencil", imageDraw_.aspect == VK_IMAGE_ASPECT_STENCIL_BIT)) {
				imageDraw_.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
			}
		} else if(subresRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
			imageDraw_.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		} else if(subresRange.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
			imageDraw_.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
		} else {
			dlg_error("Unsupported image format, aspect mask: '{}'",
				vk::flagNames(VkImageAspectFlagBits(subresRange.aspectMask)));
		}
	}

	// Row 2: layer and mip
	if(extent.depth > 1) {
		// TODO: not very convenient to use for a lot of slices.
		//   make sensitivity absolute, i.e. not dependent on number of slices?
		// TODO: this is weird when the image also has mip levels
		auto maxDepth = depth - 1;
		float layer = imageDraw_.layer * maxDepth;
		ImGui::SliderFloat("slice", &layer, 0, maxDepth);
		imageDraw_.layer /= maxDepth;
	} else if(subresRange.layerCount > 1) {
		int layer = int(imageDraw_.layer);
		ImGui::SliderInt("Layer", &layer, subresRange.baseArrayLayer,
			subresRange.baseArrayLayer + subresRange.layerCount - 1);
		imageDraw_.layer = layer;
	}

	if(subresRange.levelCount > 1) {
		int mip = int(imageDraw_.level);
		ImGui::SliderInt("Mip", &mip, subresRange.baseMipLevel,
			subresRange.baseMipLevel + subresRange.levelCount - 1);
		imageDraw_.level = mip;
	}

	// Row 3: min/max values
	ImGui::DragFloat("Min", &imageDraw_.minValue, 0.01);
	ImGui::DragFloat("Max", &imageDraw_.maxValue, 0.01);
	// NOTE: could add power/gamma slider here.

	imageDraw_.type = parseType(imgType, format, imageDraw_.aspect);
	dlg_assertm(imageDraw_.type != DrawGuiImage::font,
		"imgType {}, format {}", vk::name(imgType), vk::name(format));
}

void ImageViewer::recordPreImage(VkCommandBuffer cb) {
	auto& dev = gui_->dev();

	// prepare image for being drawn
	if(initialImageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		VkImageMemoryBarrier imgb {};
		imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imgb.image = this->src;
		imgb.subresourceRange.aspectMask = imageDraw_.aspect;
		imgb.subresourceRange.baseMipLevel = imageDraw_.level;
		imgb.subresourceRange.levelCount = 1u;
		imgb.subresourceRange.layerCount = subresRange.layerCount;
		imgb.oldLayout = initialImageLayout;
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

	auto srcLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	auto srcAccess = VK_ACCESS_SHADER_READ_BIT;
	auto srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	auto needBarrier = finalImageLayout != srcLayout;
	if(copyTexel) {
		doCopy(cb, draw, srcLayout);
		srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		needBarrier = true;
	}

	if(needBarrier) {
		auto& dev = gui_->dev();

		VkImageMemoryBarrier imgb {};
		imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imgb.image = this->src;
		imgb.subresourceRange.aspectMask = imageDraw_.aspect;
		imgb.subresourceRange.baseMipLevel = imageDraw_.level;
		imgb.subresourceRange.levelCount = 1u;
		imgb.subresourceRange.layerCount = subresRange.layerCount;
		imgb.oldLayout = srcLayout;
		imgb.newLayout = finalImageLayout;
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

	if(size_.x == 0u || size_.y == 0u) {
		return; // nothing to do
	}

	VkRect2D scissor {};
	scissor.offset.x = std::max<int>(offset_.x, 0);
	scissor.offset.y = std::max<int>(offset_.y, 0);
	scissor.extent.width = std::min<int>(
		size_.x + offset_.x - scissor.offset.x,
		displaySize.x - offset_.x);
	scissor.extent.height = std::min<int>(
		size_.y + offset_.y - scissor.offset.y,
		displaySize.y - offset_.y);
	dev.dispatch.CmdSetScissor(cb, 0, 1, &scissor);

	VkViewport viewport {};
	viewport.width = size_.x;
	viewport.height = size_.y;
	viewport.x = offset_.x;
	viewport.y = offset_.y;
	viewport.maxDepth = 1.f;
	dev.dispatch.CmdSetViewport(cb, 0, 1, &viewport);

	struct {
		Vec2f qsize;
	} pcData = {
		{20.f / size_.x, 20.f / size_.y}
	};

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		gui_->imageBgPipe());
	dev.dispatch.CmdPushConstants(cb, gui_->pipeLayout(),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(pcData), &pcData);
	dev.dispatch.CmdDraw(cb, 4, 1, 0, 0);
}

DrawGuiImage::Type ImageViewer::parseType(VkImageType imgType, VkFormat format,
		VkImageAspectFlagBits aspect) {

	// NOTE: relies on ordering of DrawGuiImage::Type enum
	auto imageTypeFUI = [](auto numt) {
		if(numt == VK_FORMAT_NUMERICAL_TYPE_SINT) return 2u;
		else if(numt == VK_FORMAT_NUMERICAL_TYPE_UINT) return 1u;
		else return 0u;
	};

	DrawGuiImage::Type baseType;
	switch(imgType) {
		case VK_IMAGE_TYPE_1D: baseType = DrawGuiImage::f1d; break;
		case VK_IMAGE_TYPE_2D: baseType = DrawGuiImage::f2d; break;
		case VK_IMAGE_TYPE_3D: baseType = DrawGuiImage::f3d; break;
		default: dlg_error("unreachable"); baseType = {}; break;
	}

	auto off = 0u;
	if(aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
		if(FormatIsSampledFloat(format)) off = 0u;
		if(FormatIsUInt(format)) off = 1u;
		if(FormatIsInt(format)) off = 2u;
	} else {
		auto numt = VK_FORMAT_NUMERICAL_TYPE_NONE;
		if(aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
			numt = FormatDepthNumericalType(format);
		} else if(aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
			numt = FormatStencilNumericalType(format);
		}

		off = imageTypeFUI(numt);
	}

	return DrawGuiImage::Type(unsigned(baseType) + off);
}

void ImageViewer::doCopy(VkCommandBuffer cb, Draw& draw, VkImageLayout srcLayout) {
	auto& dev = gui_->dev();
	dlg_assert(this->copyTexel);

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

	dlg_assert(readback->own.size >= FormatElementSize(format));

	// TODO: fix handling of depth-stencil images. We probably have to pass
	// multiple copy regions (one for each aspect, see the docs of
	// VkBufferImageCopy). But then also fix the aspects in the barriers.

	VkImageMemoryBarrier imgb {};
	imgb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgb.image = this->src;
	imgb.subresourceRange.aspectMask = imageDraw_.aspect;
	imgb.subresourceRange.baseMipLevel = imageDraw_.level;
	imgb.subresourceRange.levelCount = 1u;
	imgb.subresourceRange.layerCount = subresRange.layerCount;
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
	copy.imageSubresource.aspectMask = imageDraw_.aspect;
	copy.imageSubresource.mipLevel = imageDraw_.level;
	copy.imageSubresource.layerCount = 1u;
	dlg_assert(readTexelOffset_.x >= 0 && readTexelOffset_.y >= 0);
	[[maybe_unused]] auto w = std::max(extent.width >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto h = std::max(extent.height >> u32(imageDraw_.level), 1u);
	dlg_assert(u32(readTexelOffset_.x) < w && u32(readTexelOffset_.y) < h);

	copy.imageOffset.x = readTexelOffset_.x;
	copy.imageOffset.y = readTexelOffset_.y;
	if(imgType == VK_IMAGE_TYPE_3D) {
		copy.imageOffset.z = imageDraw_.layer;
	} else {
		copy.imageSubresource.baseArrayLayer = imageDraw_.layer;
	}

	dev.dispatch.CmdCopyImageToBuffer(cb, src,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->own.buf, 1, &copy);

	readback->valid = true; // TODO: reset on select
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
			}
		}
	};

	draw.onFinish.push_back(cbFinish);
}

} // namespace vil
