#include <gui/imageViewer.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <util/util.hpp>
#include <util/fmt.hpp>
#include <util/vecOps.hpp>
#include <device.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/format_utils.h>
#include <vkutil/enumString.hpp>
#include <vkutil/sync.hpp>
#include <vkutil/cmd.hpp>

namespace vil {

// see histogram.glsl
struct MinMaxData {
	Vec4u32 texMin;
	Vec4u32 texMax;
	u32 flags;
};

// See minmax.comp
const u32 flagHasNan = 1u;
const u32 flagHasInf = 2u;

float uintOrderedToFloat(u32 val) {
	if((val & (1u << 31)) == 0) {
		// original float must have been negative
		return bit_cast<float>(val ^ 0xFFFFFFFFu);
	} else {
		// original float must have been positive
		return bit_cast<float>(val ^ 0x80000000u);
	}
}

void appendAsInt(std::string& str, double val) {
	str += std::to_string(i64(val));
}

void appendAsDouble(std::string& str, double val) {
	str += std::to_string(val);
}

std::string format(VkFormat format, VkImageAspectFlagBits aspect, Vec4d value) {
	using FmtFunc = void(*)(std::string&, double);
	using NumType = FORMAT_NUMERICAL_TYPE;
	FmtFunc fmt;
	unsigned channels;

	if(aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
		channels = FormatComponentCount(format);
		fmt = (FormatIsSampledFloat(format)) ? appendAsDouble : appendAsInt;
	} else if(aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
		channels = 1u;
		auto numt = FormatDepthNumericalType(format);
		fmt = (numt == NumType::UINT || numt == NumType::SINT) ?
			appendAsInt :
			appendAsDouble;
	} else if(aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
		channels = 1u;
		auto numt = FormatStencilNumericalType(format);
		fmt = (numt == NumType::UINT || numt == NumType::SINT) ?
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

void ImageViewer::drawMetaInfo(Draw& draw) {
	(void) draw;

	// Row 1: components
	auto recreateView = false;
	VkFlags depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	if(subresRange_.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		auto numComponents = FormatComponentCount(format_);
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

	const auto level = u32(imageDraw_.level);
	// const auto width = std::max(extent_.width >> level, 1u);
	// const auto height = std::max(extent_.height >> level, 1u);
	const auto depth = std::max(extent_.depth >> level, 1u);

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
}

void ImageViewer::drawImageArea(Draw& draw) {
	ImVec2 pos = ImGui::GetCursorScreenPos();

	auto level = u32(imageDraw_.level);
	auto width = std::max(extent_.width >> level, 1u);
	auto height = std::max(extent_.height >> level, 1u);
	[[maybe_unused]] auto depth = std::max(extent_.depth >> level, 1u);

	const float aspectImage = float(width) / height;
	float regW = ImGui::GetContentRegionAvail().x;

	// kinda messy but need this to make avoid flickering for
	// windows that barely need a scrollbar (adding a scrollbar makes
	// the image smaller, causing the content to not need a scrollbar
	// anymore; we will get flickering).
	auto* win = ImGui::GetCurrentWindowRead();
	if(win->ScrollbarY) {
		regW += ImGui::GetStyle().ScrollbarSize;
	}

	float regH = regW / aspectImage;

	// TODO: proper computation using imgui margin/padding values
	auto bottomLineHeight = 10.f + ImGui::GetFontSize();
	regH = ImGui::GetContentRegionAvail().y - bottomLineHeight;

	auto bgW = regW;
	auto bgH = regH;

	// This is needed for HUGEx1 images (e.g. 1024x1) or 1 dimensional images.
	// We make sure there is at least some area to zoom in.
	bgH = std::max(bgH, 100.f);

	// TODO: don't cut off

	// Cut the image off if too high
	if(regW > regH) {
		// bgW = bgH;
		regW = bgW;
		regW = regH * aspectImage;
	} else if(regH > regW) {
		// bgH = bgW;
		regH = bgH;
		regH = regW / aspectImage;
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
	auto owner = [drawData = data_](Draw&, bool) mutable { drawData.reset(); };
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

	// If this is false we show "Reading..." and the current mouse position
	// instead of the position and texel value of the last read texel.
	// I'd argue showing slightly outdated (position-wise) data has more
	// value than just showing "Reading..." on cursor movement.
	constexpr auto showOutdatedPos = true;

	bool texelValid = false;
	if(lastReadback_) {
		auto& rb = readbacks_[*lastReadback_];
		dlg_assert(!rb.pending);
		if(rb.valid && (showOutdatedPos || (
				rb.texel.x == readTexelOffset_.x &&
				rb.texel.y == readTexelOffset_.y)) &&
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

			imGuiText("Position: {}, {} | Format {}",
				rb.texel.x, rb.texel.y,
				vk::name(format_));
			ImGui::SameLine();
			imGuiText("| Texel: {}", format(format_, aspect_, value));

			auto data = rb.own.data();
			skip(data, 100u);
			auto minMax = read<MinMaxData>(data);
			ImGui::SameLine();
			Vec4f min;
			Vec4f max;
			for(auto i = 0u; i < 4; ++i) {
				min[i] = uintOrderedToFloat(minMax.texMin[i]);
				max[i] = uintOrderedToFloat(minMax.texMax[i]);
			}

			imGuiText("| Min: {}, Max: {}", min, max);

			if(minMax.flags != 0u) {
				if(minMax.flags & flagHasInf) {
					minMax.flags &= ~flagHasInf;
					ImGui::SameLine();
					imGuiText("| HasInf");
				}
				if(minMax.flags & flagHasNan) {
					minMax.flags &= ~flagHasNan;
					ImGui::SameLine();
					imGuiText("| HasNan");
				}
				if(minMax.flags != 0u) {
					ImGui::SameLine();
					imGuiText("| Unexpected flag: {}", minMax.flags);
				}
			}
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

		imGuiText("Position: {}, {} | Format {}",
			readTexelOffset_.x, readTexelOffset_.y,
			vk::name(format_));
		ImGui::SameLine();

		if(copyTexel_) {
			imGuiText("| Texel: Reading...");
		} else {
			imGuiText("| Texel: Can't read Image");
		}
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

void ImageViewer::display(Draw& draw) {
	if(!data_) {
		dlg_error("ImageViewer::display without selected image");
		imGuiText("Error: no select image");
		return;
	}

	dlg_assert(src_);

	// find free readback or create a new one
	auto& dev = gui_->dev();
	Readback* readback {};
	for(auto [i, r] : enumerate(readbacks_)) {
		if(!r.pending && (!lastReadback_ || i != *lastReadback_)) {
			readback = &r;
			break;
		}
	}

	if(!readback) {
		readback = &readbacks_.emplace_back();

		auto maxBufSize = 1024;
		readback->own.ensure(dev, maxBufSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		readback->opDS = gui_->allocDs(gui_->imgOpDsLayout(), "ImageViewer:readbackOpDs");
	}

	dlg_assert(FormatElementSize(format_) <= 100u);

	readback->valid = true;
	readback->pending = &draw;

	// register callback to be called when gpu batch finishes execution
	auto cbFinish = [this](Draw& draw, bool success) {
		auto found = false;
		for(auto [i, readback] : enumerate(readbacks_)) {
			if(readback.pending == &draw) {
				dlg_assert(!found);
				found = true;
				readback.pending = nullptr;

				if(success) {
					lastReadback_ = i;
				}

				// break;
			}
		}

		dlg_assert(found);
	};

	draw.onFinish.push_back(cbFinish);

	gui_->addPreRender([=](Draw& draw) { this->recordPreImage(draw, *readback); });
	gui_->addPostRender([=](Draw& draw) { this->recordPostImage(draw, *readback); });

	auto level = u32(imageDraw_.level);
	auto width = std::max(extent_.width >> level, 1u);
	auto height = std::max(extent_.height >> level, 1u);

	const float availX = ImGui::GetContentRegionAvail().x;
	const float availY = ImGui::GetContentRegionAvail().y;

	const float aspectImage = float(width) / height;
	const float aspectAvail = availX / availY;

	if(aspectImage > aspectAvail) {
		auto availY = (availX - 30) / aspectImage;
		if(ImGui::BeginChild("ImageArea", ImVec2(availX - 30, availY))) {
			drawImageArea(draw);
			ImGui::EndChild();
		}

		drawMetaInfo(draw);
	} else {
		auto flags = ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_Resizable;
		if(ImGui::BeginTable("Img", 2u, flags)) {
			ImGui::TableSetupColumn("col0", ImGuiTableColumnFlags_WidthStretch, 1.f);
			ImGui::TableSetupColumn("col1", ImGuiTableColumnFlags_WidthStretch, 0.3f);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			if(ImGui::BeginChild("ImageArea", ImVec2(0.f, availY))) {
				drawImageArea(draw);
				ImGui::EndChild();
			}

			ImGui::TableNextColumn();

			drawMetaInfo(draw);

			ImGui::EndTable();
		}
	}
}

void ImageViewer::recordPreImage(Draw& draw, Readback& rb) {
	auto& dev = gui_->dev();
	DebugLabel cblbl(gui_->dev(), draw.cb, "vil:ImageViewer:pre");

	vku::LocalImageState srcState;
	srcState.image = src_;
	srcState.lastAccess = vku::SyncScope::allAccess(initialImageLayout_);
	srcState.range = subresRange_;

	computeMinMax(draw, srcState, rb);

	// prepare image for being drawn
	if(initialImageLayout_ != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		srcState.transition(dev, draw.cb, vku::SyncScope::fragmentRead());
	}
}

void ImageViewer::recordPostImage(Draw& draw, Readback& rb) {
	auto cb = draw.cb;
	DebugLabel cblbl(gui_->dev(), cb, "vil:ImageViewer:post");

	vku::LocalImageState srcState;
	srcState.image = src_;
	srcState.lastAccess = vku::SyncScope::fragmentRead();
	srcState.range = subresRange_;

	if(copyTexel_) {
		// We want to get a single texel from the texture on the cpu.
		// For many formats this can be done by simple copying it to our
		// hostVisible buffer. But for compressed formats this is more
		// complicated, we therefore use sampling by default.
		// Don't delete the doCopy codepath, there might be cases in
		// which we need it (some hardware not supporting sampling
		// certain formats? maybe copying is a lot faster sometimes, making
		// a dynamic branch here worth it?)
		constexpr auto useSamplingCopy = true;
		if(useSamplingCopy) {
			doSample(draw, srcState, rb);
		} else {
			doCopy(draw, srcState, rb);
		}
	}

	srcState.transition(gui_->dev(), cb, vku::SyncScope::allAccess(finalImageLayout_));
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
	dev.dispatch.CmdPushConstants(cb, gui_->pipeLayout().vkHandle(),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(pcData), &pcData);
	dev.dispatch.CmdDraw(cb, 4, 1, 0, 0);
}

void ImageViewer::doSample(Draw& draw, vku::LocalImageState& srcState, Readback& rb) {
	auto cb = draw.cb;
	auto& dev = gui_->dev();
	dlg_assert(this->copyTexel_);

	dlg_assert(FormatElementSize(format_) <= 100);
	srcState.transition(dev, cb, vku::SyncScope::fragmentRead());

	// update ds
	vku::DescriptorUpdate(rb.opDS)
		(rb.own.asSpan())
		(data_->view.vkHandle(), dev.nearestSampler);

	// prepare sample copy operation
	auto& pipe = gui_->readTexPipe(imageDraw_.type);
	auto& pipeLayout = gui_->imgOpPipeLayout().vkHandle();

	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

	u32 layer = imageDraw_.layer;
	u32 level = imageDraw_.level;
	Vec3i coords{readTexelOffset_.x, readTexelOffset_.y, 0};

	if(imgType_ == VK_IMAGE_TYPE_3D) {
		layer = 0;
		coords.z = int(imageDraw_.layer);
	}

	validateClampCoords(coords, layer, level);

	struct {
		Vec3i coords;
		int level;
	} pcr;

	pcr.level = level;
	pcr.coords = coords;

	if(imgType_ == VK_IMAGE_TYPE_1D) {
		pcr.coords.y = layer;
		pcr.coords.z = 0; // z irrelevant
	} else if(imgType_ == VK_IMAGE_TYPE_2D) {
		pcr.coords.z = layer;
	}

	dev.dispatch.CmdPushConstants(cb, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
		0u, sizeof(pcr), &pcr);
	dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipeLayout, 0, 1, &rb.opDS.vkHandle(), 0, nullptr);
	dev.dispatch.CmdDispatch(cb, 1, 1, 1);

	rb.level = level;
	rb.layer = layer;
	rb.texel = readTexelOffset_;
}

void ImageViewer::doCopy(Draw& draw, vku::LocalImageState& srcState, Readback& rb) {
	auto cb = draw.cb;
	auto& dev = gui_->dev();
	dlg_assert(this->copyTexel_);
	dlg_assertm(FormatTexelBlockExtent(format_).width == 1u,
		"Block formats not supported for copying");

	// TODO: fix handling of depth-stencil images. We probably have to pass
	// multiple copy regions (one for each aspect, see the docs of
	// VkBufferImageCopy). But then also fix the aspects in the barriers.
	// TODO: transfer queue ownership?
	// We currently just force concurrent mode on image/buffer creation
	// but that might have performance impact.
	// Requires additional submissions to the other queues.
	// We should first check whether the queue is different in first place.
	// if(img.ci.sharingMode == VK_SHARING_MODE_EXCLUSIVE) {
	// }

	srcState.transition(dev, cb, vku::SyncScope::transferRead());

	// get valid coords
	u32 layer = imageDraw_.layer;
	u32 level = imageDraw_.level;
	Vec3i coords{readTexelOffset_.x, readTexelOffset_.y, 0};

	if(imgType_ == VK_IMAGE_TYPE_3D) {
		layer = 0;
		coords.z = int(imageDraw_.layer);
	}

	validateClampCoords(coords, layer, level);

	// copy
	VkBufferImageCopy copy {};
	copy.imageExtent = {1, 1, 1};
	copy.imageSubresource.aspectMask = aspect_;
	copy.imageSubresource.mipLevel = level;
	copy.imageSubresource.baseArrayLayer = layer;
	copy.imageSubresource.layerCount = 1u;

	copy.imageOffset.x = coords.x;
	copy.imageOffset.y = coords.y;
	copy.imageOffset.z = coords.z;

	dev.dispatch.CmdCopyImageToBuffer(cb, src_,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.own.buf, 1, &copy);

	rb.level = imageDraw_.level;
	rb.layer = imageDraw_.layer;
	rb.texel = readTexelOffset_;
}

void ImageViewer::computeMinMax(Draw& draw, vku::LocalImageState& srcState,
		Readback& rb) {
	auto& dev = gui_->dev();
	srcState.transition(dev, draw.cb, vku::SyncScope::computeRead());

	vku::LocalBufferState histBufState{data_->histogram.asSpan()};
	histBufState.discard(dev, draw.cb, vku::SyncScope::transferWrite());

	const u32 minVal = 0xFFFFFFFFu;
	const u32 maxVal = 0x00000000u;

	MinMaxData data {};
	data.texMin = {minVal, minVal, minVal, minVal};
	data.texMax = {maxVal, maxVal, maxVal, maxVal};
	dev.dispatch.CmdUpdateBuffer(draw.cb, data_->histogram.buf,
		0u, sizeof(data), &data);

	histBufState.transition(dev, draw.cb, vku::SyncScope::computeReadWrite());

	auto pipeLayout = gui_->imgOpPipeLayout().vkHandle();
	auto pipe = gui_->pipes().minMaxTex[imageDraw_.type];

	dev.dispatch.CmdBindPipeline(draw.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	dev.dispatch.CmdBindDescriptorSets(draw.cb,
		VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout,
		0u, 1u, &data_->histDs.vkHandle(), 0u, nullptr);

	int level = int(imageDraw_.level);
	dev.dispatch.CmdPushConstants(draw.cb, pipeLayout,
		VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(level), &level);

	auto w = std::max(extent_.width >> u32(level), 1u);
	auto h = std::max(extent_.height >> u32(level), 1u);
	auto d = std::max(extent_.depth >> u32(level), 1u);

	dev.dispatch.CmdDispatch(draw.cb, ceilDivide(w, 8u), ceilDivide(h, 8u), d);

	// we want min/max and flags on cpu as well
	histBufState.transition(dev, draw.cb, vku::SyncScope::transferRead());
	vku::cmdCopyBuffer(dev, draw.cb,
		data_->histogram.asSpan(0u, sizeof(MinMaxData)),
		rb.own.asSpan(100u));
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
				auto numComponents = FormatComponentCount(format_);
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

	dlg_assert(aspect_ == VK_IMAGE_ASPECT_COLOR_BIT ||
		imageDraw_.flags == (DrawGuiImage::flagMaskR | DrawGuiImage::flagGrayscale));

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

void ImageViewer::reset(bool resetZoomPanSelection) {
	unselect();
	imageDraw_.ds = {};

	if(resetZoomPanSelection) {
		imageDraw_ = {};
		offset_ = {};
		scale_ = 1.f;
		aspect_ = {};
		subresRange_ = {};
	}
}

void ImageViewer::unselect() {
	src_ = {};
	draw_ = {};
	lastReadback_ = {};
}

void ImageViewer::createData() {
	auto& dev = gui_->dev();

	data_.reset(new DrawData());
	data_->gui = gui_;

	VkImageViewCreateInfo ivi {};
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.image = src_;
	ivi.viewType = imageViewForImageType(imgType_);
	ivi.format = format_;
	ivi.subresourceRange = subresRange_;
	ivi.subresourceRange.aspectMask = aspect_;
	data_->view = {dev, ivi, "ImageViewer:imageView"};

	data_->ds = gui_->allocDs(gui_->imguiDsLayout(), "ImageViewer:display");
	// TODO: use better sampler, with linear interpolation where possible.
	// But only when image is too large for canvas, otherwise use nearest via
	// min/magFilter
	vku::DescriptorUpdate(data_->ds)
		(data_->view.vkHandle(), dev.nearestSampler);

	// TODO: don't just hardcode size here
	data_->histogram.ensure(dev, 8192,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, {}, OwnBuffer::Type::deviceLocal);
	data_->histDs = gui_->allocDs(gui_->imgOpDsLayout(), "ImageViewer:histDs");
	vku::DescriptorUpdate(data_->histDs)
		(data_->histogram.asSpan())
		(data_->view.vkHandle(), dev.nearestSampler);

	imageDraw_.ds = data_->ds.vkHandle();
}

void ImageViewer::validateClampCoords(Vec3i& coords, u32& layer, u32& level) {
	// things here shouldn't go wrong (but often did end up to somehow
	// in the past) and assertions should be fixed.
	[[maybe_unused]] auto layerBegin = subresRange_.baseArrayLayer;
	[[maybe_unused]] auto layerEnd = layerBegin + subresRange_.layerCount;
	[[maybe_unused]] auto levelBegin = subresRange_.baseMipLevel;
	[[maybe_unused]] auto levelEnd = levelBegin + subresRange_.levelCount;
	[[maybe_unused]] auto w = std::max(extent_.width >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto h = std::max(extent_.height >> u32(imageDraw_.level), 1u);
	[[maybe_unused]] auto d = std::max(extent_.depth >> u32(imageDraw_.level), 1u);

	dlg_assert_or(coords.x >= 0, coords.x = 0);
	dlg_assert_or(coords.y >= 0, coords.y = 0);
	dlg_assert_or(u32(coords.x) < w, coords.x = w - 1);
	dlg_assert_or(u32(coords.y) < h, coords.y = h - 1);
	dlg_assert_or(u32(coords.z) < d, coords.z = d - 1);
	dlg_assert_or(layer >= layerBegin, layer = layerBegin);
	dlg_assert_or(layer < layerEnd, layer = layerEnd - 1);
	dlg_assert_or(level >= levelBegin, level = levelBegin);
	dlg_assert_or(level < levelEnd, level = levelEnd - 1);
}

} // namespace vil
