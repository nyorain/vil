#include <gui/imageViewer.hpp>
#include <gui/gui.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/format_utils.h>
#include <vk/enumString.hpp>

namespace vil {

void ImageViewer::init(Gui& gui) {
	gui_ = &gui;
}

void ImageViewer::display(Draw& draw) {
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

	// draw image
	// background
	draw_ = &draw;
	auto cb = [](const ImDrawList*, const ImDrawCmd* cmd) {
		auto* self = static_cast<ImageViewer*>(cmd->UserCallbackData);
		dlg_assert(self->draw_);
		self->drawBackground(self->draw_->cb);
	};

	ImGui::GetWindowDrawList()->AddCallback(cb, this);

	// image
	ImGui::PushClipRect({pos.x, pos.y}, {pos.x + bgW, pos.y + bgH}, true);

	auto clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
	auto clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
	offset_ = {clipMin.x, clipMin.y};
	size_ = {clipMax.x - clipMin.x, clipMax.y - clipMin.y};

	auto uv0 = ImVec2(-this->offset.x / scale, -this->offset.y / scale);
	auto uv1 = ImVec2((1 - this->offset.x) / scale, (1 - this->offset.y) / scale);
	uv1.x *= bgW / float(regW);
	uv1.y *= bgH / float(regH);
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

} // namespace vil
