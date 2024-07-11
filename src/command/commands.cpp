#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <command/match.hpp>
#include <shader.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <sync.hpp>
#include <cb.hpp>
#include <pipe.hpp>
#include <ds.hpp>
#include <queryPool.hpp>
#include <rp.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <nytl/span.hpp>
#include <util/util.hpp>
#include <util/ext.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/cb.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/format_utils.h>
#include <vkutil/enumString.hpp>
#include <iomanip>
#include <filesystem>

namespace vil {

// Command utility
template<typename C>
auto rawHandles(ThreadMemScope& scope, const C& handles) {
	using VkH = decltype(handles[0]->handle);
	auto ret = scope.alloc<VkH>(handles.size());
	for(auto i = 0u; i < handles.size(); ++i) {
		ret[i] = handles[i]->handle;
	}

	return ret;
}

// ArgumentsDesc
NameResult nameRes(Handle* handle, VkObjectType objectType,
		NullName nullName, bool displayType) {
	if(!handle) {
		switch(nullName) {
			// TODO: also add type here when displayType is true?
			case NullName::null: return {NameType::null, "<null>"};
			case NullName::destroyed: return {NameType::null, "<destroyed>"};
			case NullName::empty: return {NameType::null, ""};

		}
	}

	auto name = vil::name(*handle, objectType, displayType);
	if(handle->name.empty()) {
		return {NameType::unnamed, name};
	}

	return {NameType::named, name};
}

// copy util
std::string printImageOffset(Image* img, const VkOffset3D& offset) {
	if(img && img->ci.imageType == VK_IMAGE_TYPE_1D) {
		return dlg::format("{}", offset.x);
	} else if(img && img->ci.imageType == VK_IMAGE_TYPE_2D) {
		return dlg::format("{}, {}", offset.x, offset.y);
	} else {
		return dlg::format("{}, {}, {}", offset.x, offset.y, offset.z);
	}
}

std::string printImageSubresLayers(Image* img, const VkImageSubresourceLayers& subres) {
	std::string subresStr;
	auto sepStr = "";
	if(!img || img->ci.mipLevels > 1) {
		subresStr = dlg::format("{}mip {}", sepStr, subres.mipLevel);
		sepStr = ", ";
	}

	if(!img || img->ci.arrayLayers > 1) {
		if(subres.layerCount > 1) {
			subresStr = dlg::format("{}layers {}..{}", sepStr,
				subres.baseArrayLayer, subres.baseArrayLayer + subres.layerCount - 1);
		} else {
			subresStr = dlg::format("{}layer {}", sepStr, subres.baseArrayLayer);
		}

		sepStr = ", ";
	}

	return subresStr;
}

std::string printImageRegion(Image* img, const VkOffset3D& offset,
		const VkImageSubresourceLayers& subres) {

	auto offsetStr = printImageOffset(img, offset);
	auto subresStr = printImageSubresLayers(img, subres);

	auto sep = subresStr.empty() ? "" : ", ";
	return dlg::format("({}{}{})", offsetStr, sep, subresStr);
}

std::string printBufferImageCopy(Image* image,
		const VkBufferImageCopy2KHR& copy, bool bufferToImage) {
	auto imgString = printImageRegion(image, copy.imageOffset, copy.imageSubresource);

	std::string sizeString;
	if(image && image->ci.imageType == VK_IMAGE_TYPE_1D) {
		sizeString = dlg::format("{}", copy.imageExtent.width);
	} else if(image && image->ci.imageType <= VK_IMAGE_TYPE_2D) {
		sizeString = dlg::format("{} x {}", copy.imageExtent.width,
			copy.imageExtent.height);
	} else {
		sizeString = dlg::format("{} x {} x {}", copy.imageExtent.width,
			copy.imageExtent.height, copy.imageExtent.depth);
	}

	auto bufString = dlg::format("offset {}", copy.bufferOffset);
	if(copy.bufferRowLength || copy.bufferImageHeight) {
		bufString += dlg::format(", rowLength {}, imageHeight {}",
			copy.bufferRowLength, copy.bufferImageHeight);
	}

	if(bufferToImage) {
		return dlg::format("({}) -> {} [{}]", bufString, imgString, sizeString);
	} else {
		return dlg::format("({}) -> {} [{}]", imgString, bufString, sizeString);
	}
}

// Command
bool Command::isChild(const Command& cmd) const {
	auto* it = children();
	while(it) {
		if(it == &cmd) {
			return true;
		}

		it = it->next;
	}

	return false;
}

bool Command::isDescendant(const Command& cmd) const {
	auto* it = children();
	while(it) {
		if(it == &cmd || it->isDescendant(cmd)) {
			return true;
		}

		it = it->next;
	}

	return false;
}

std::string formatQueueFam(u32 fam) {
	if(fam == VK_QUEUE_FAMILY_IGNORED) {
		return "ignored";
	} else if(fam == VK_QUEUE_FAMILY_EXTERNAL) {
		return "external";
	} else if(fam == VK_QUEUE_FAMILY_FOREIGN_EXT) {
		return "foreign";
	}

	return std::to_string(fam);
}

// dummy for validExpression below
template<typename B> using SrcStageMaskMember = decltype(B::srcStageMask);

template<typename ImageBarrier, typename BufferBarrier, typename MemoryBarrier>
void displayBarriers(
		Gui& gui, span<Image*> images, span<Buffer*> buffers,
		span<ImageBarrier> imgBarriers,
		span<BufferBarrier> bufBarriers,
		span<MemoryBarrier> memBarriers) {
	constexpr auto v2 = validExpression<SrcStageMaskMember, MemoryBarrier>;

	if(!memBarriers.empty()) {
		imGuiText("Memory Barriers");
		ImGui::Indent();
		for(auto i = 0u; i < memBarriers.size(); ++i) {
			auto& memb = memBarriers[i];

			if constexpr(v2) {
				imGuiText("srcStage: {}", vk::namePipelineStageFlags2(memb.srcStageMask));
				imGuiText("dstStage: {}", vk::namePipelineStageFlags2(memb.dstStageMask));
			}

			imGuiText("srcAccess: {}", vk::nameAccessFlags2(memb.srcAccessMask));
			imGuiText("dstAccess: {}", vk::nameAccessFlags2(memb.dstAccessMask));

			ImGui::Separator();
		}
		ImGui::Unindent();
	}

	if(!bufBarriers.empty()) {
		imGuiText("Buffer Barriers");
		ImGui::Indent();
		for(auto i = 0u; i < bufBarriers.size(); ++i) {
			auto& bufb = bufBarriers[i];
			refButtonD(gui, buffers[i]);

			imGuiText("offset: {}", bufb.offset);
			imGuiText("size: {}", bufb.size);

			if constexpr(v2) {
				imGuiText("srcStage: {}", vk::namePipelineStageFlags2(bufb.srcStageMask));
				imGuiText("dstStage: {}", vk::namePipelineStageFlags2(bufb.dstStageMask));
			}

			imGuiText("srcAccess: {}", vk::nameAccessFlagBits2(bufb.srcAccessMask));
			imGuiText("dstAccess: {}", vk::nameAccessFlagBits2(bufb.dstAccessMask));
			imGuiText("srcQueueFamily: {}", formatQueueFam(bufb.srcQueueFamilyIndex));
			imGuiText("dstQueueFamily: {}", formatQueueFam(bufb.dstQueueFamilyIndex));
			ImGui::Separator();
		}
		ImGui::Unindent();
	}

	if(!imgBarriers.empty()) {
		imGuiText("Image Barriers");
		ImGui::Indent();
		for(auto i = 0u; i < imgBarriers.size(); ++i) {
			auto& imgb = imgBarriers[i];
			refButtonD(gui, images[i]);

			auto& subres = imgb.subresourceRange;
			imGuiText("aspectMask: {}", vk::nameImageAspectFlags(subres.aspectMask));
			imGuiText("baseArrayLayer: {}", subres.baseArrayLayer);
			imGuiText("layerCount: {}", subres.layerCount);
			imGuiText("baseMipLevel: {}", subres.baseMipLevel);
			imGuiText("levelCount: {}", subres.levelCount);

			if constexpr(v2) {
				imGuiText("srcStage: {}", vk::namePipelineStageFlags2(imgb.srcStageMask));
				imGuiText("dstStage: {}", vk::namePipelineStageFlags2(imgb.dstStageMask));
			}

			imGuiText("srcAccess: {}", vk::nameAccessFlags(imgb.srcAccessMask));
			imGuiText("dstAccess: {}", vk::nameAccessFlags(imgb.dstAccessMask));
			imGuiText("oldLayout: {}", vk::name(imgb.oldLayout));
			imGuiText("newLayout: {}", vk::name(imgb.newLayout));
			imGuiText("srcQueueFamily: {}", formatQueueFam(imgb.srcQueueFamilyIndex));
			imGuiText("dstQueueFamily: {}", formatQueueFam(imgb.dstQueueFamilyIndex));
			ImGui::Separator();
		}
		ImGui::Unindent();
	}
}

// match ideas:
// - matching for bitmask flags
// - matching for sorted spans
// - multiplicative matching addition?
//   basically saying "if this doesn't match, the whole command shouldn't match,
//   even if everything else does"
//   Different than just using a high weight in that a match doesn't automatically
//   mean a match for the whole command.

Command::Command() {
}

// BarrierCmdBase
template<typename ImageBarrier>
void patch(ImageBarrier& ib, u32 recordQueueFamilyIndex) {
	// For queue family ownership transitions we need to ignore
	// one of the layout transitions. We just choose to always ignore
	// the acquire transition.
	if(ib.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
			ib.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
			ib.srcQueueFamilyIndex != ib.dstQueueFamilyIndex) {
		auto ignoreLayoutTransition =
			ib.dstQueueFamilyIndex == recordQueueFamilyIndex;
		if(ignoreLayoutTransition) {
			// we know it's an acquire barrier and the layout
			// transition was previously done
			ib.oldLayout = ib.newLayout;
		}
	}

	// ignore queue family ownership transition
	ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
}

void BarrierCmdBase::displayInspector(Gui& gui) const {
	imGuiText("srcStage: {}", vk::namePipelineStageFlags2(srcStageMask));
	imGuiText("dstStage: {}", vk::namePipelineStageFlags2(dstStageMask));

	displayBarriers(gui, images, buffers,
		imgBarriers, bufBarriers, memBarriers);
}

BarrierCmdBase::PatchedBarriers BarrierCmdBase::patchedBarriers(
		ThreadMemScope& memScope, u32 qfam) const {
	PatchedBarriers ret;
	ret.memBarriers = memBarriers; // need no patching
	ret.imgBarriers = memScope.copy(imgBarriers);
	ret.bufBarriers = memScope.copy(bufBarriers);

	for(auto i = 0u; i < ret.imgBarriers.size(); ++i) {
		if(!images[i]->concurrentHooked) {
			continue;
		}

		patch(ret.imgBarriers[i], qfam);
	}

	for(auto i = 0u; i < ret.bufBarriers.size(); ++i) {
		if(!buffers[i]->concurrentHooked) {
			continue;
		}

		auto& bb = ret.bufBarriers[i];

		// ignore queue family ownership transition
		bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}

	return ret;
}

// BarrierCmdBase2
void Barrier2CmdBase::displayInspector(Gui& gui) const {
	displayBarriers(gui, images, buffers,
		imgBarriers, bufBarriers, memBarriers);
}

VkDependencyInfo Barrier2CmdBase::patchedBarriers(
		ThreadMemScope& memScope, u32 qfam) const {
	auto imgbPatched = memScope.copy(imgBarriers);
	auto bufbPatched = memScope.copy(bufBarriers);

	for(auto i = 0u; i < imgbPatched.size(); ++i) {
		if(!images[i]->concurrentHooked) {
			continue;
		}

		patch(imgbPatched[i], qfam);
	}

	for(auto i = 0u; i < bufbPatched.size(); ++i) {
		if(!buffers[i]->concurrentHooked) {
			continue;
		}

		auto& bb = bufbPatched[i];

		// ignore queue family ownership transition
		bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}

	VkDependencyInfo ret {};
	ret.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	ret.dependencyFlags = flags;
	ret.bufferMemoryBarrierCount = bufbPatched.size();
	ret.pBufferMemoryBarriers = bufbPatched.data();
	ret.imageMemoryBarrierCount = imgbPatched.size();
	ret.pImageMemoryBarriers = imgbPatched.data();
	// no need to modify memory barriers
	ret.memoryBarrierCount = memBarriers.size();
	ret.pMemoryBarriers = memBarriers.data();

	return ret;
}

// WaitEventsCmd
void WaitEventsCmd::record(const Device& dev, VkCommandBuffer cb, u32 qfam) const {
	ThreadMemScope memScope;
	auto vkEvents = rawHandles(memScope, this->events);
	auto [memb, bufb, imgb] = patchedBarriers(memScope, qfam);

	dev.dispatch.CmdWaitEvents(cb,
		u32(vkEvents.size()), vkEvents.data(),
		this->srcStageMask, this->dstStageMask,
		u32(memb.size()), memb.data(),
		u32(bufb.size()), bufb.data(),
		u32(imgb.size()), imgb.data());
}

void WaitEventsCmd::displayInspector(Gui& gui) const {
	for(auto& event : events) {
		refButtonD(gui, event);
	}

	BarrierCmdBase::displayInspector(gui);
}

// WaitEvents2Cmd
void WaitEvents2Cmd::record(const Device& dev, VkCommandBuffer cb, u32 qfam) const {
	ThreadMemScope memScope;
	auto vkEvents = rawHandles(memScope, this->events);
	auto dep = patchedBarriers(memScope, qfam);
	dev.dispatch.CmdWaitEvents2(cb, u32(vkEvents.size()), vkEvents.data(), &dep);
}

void WaitEvents2Cmd::displayInspector(Gui& gui) const {
	for(auto& event : events) {
		refButtonD(gui, event);
	}

	Barrier2CmdBase::displayInspector(gui);
}

// BarrierCmd
void BarrierCmd::record(const Device& dev, VkCommandBuffer cb, u32 qfam) const {
	ThreadMemScope ms;
	auto [memb, bufb, imgb] = patchedBarriers(ms, qfam);

	dev.dispatch.CmdPipelineBarrier(cb,
		this->srcStageMask, this->dstStageMask, this->dependencyFlags,
		u32(memb.size()), memb.data(),
		u32(bufb.size()), bufb.data(),
		u32(imgb.size()), imgb.data());
}

void BarrierCmd::displayInspector(Gui& gui) const {
	imGuiText("dependencyFlags: {}", vk::nameDependencyFlags(dependencyFlags));
	BarrierCmdBase::displayInspector(gui);
}

// BarrierCmd2
void Barrier2Cmd::record(const Device& dev, VkCommandBuffer cb, u32 qfam) const {
	ThreadMemScope ms;
	auto dep = patchedBarriers(ms, qfam);
	dev.dispatch.CmdPipelineBarrier2(cb, &dep);
}

void Barrier2Cmd::displayInspector(Gui& gui) const {
	Barrier2CmdBase::displayInspector(gui);
}

// BeginRenderPassCmd
unsigned BeginRenderPassCmd::subpassOfDescendant(const Command& cmd) const {
	auto subpass = this->children();
	for(auto i = 0u; subpass; ++i, subpass = subpass->next) {
		if(subpass->isDescendant(cmd)) {
			return i;
		}
	}

	return u32(-1);
}

std::string BeginRenderPassCmd::toString() const {
	auto [fbRes, fbName] = nameRes(fb);
	auto [rpRes, rpName] = nameRes(rp);
	if(fbRes == NameType::named && rpRes == NameType::named) {
		return dlg::format("BeginRenderPass({}, {})", rpName, fbName);
	} else if(rpRes == NameType::named) {
		return dlg::format("BeginRenderPass({})", rpName);
	} else {
		return "BeginRenderPass";
	}
}

void BeginRenderPassCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	// NOTE: since we always manually re-record secondary command buffers,
	// we must always pass VK_SUBPASS_CONTENTS_INLINE here.
	auto info = this->subpassBeginInfo;
	info.contents = VK_SUBPASS_CONTENTS_INLINE;

	if(this->subpassBeginInfo.pNext) {
		auto f = dev.dispatch.CmdBeginRenderPass2;
		dlg_assert(f);
		f(cb, &this->info, &info);
	} else {
		dev.dispatch.CmdBeginRenderPass(cb, &this->info, info.contents);
	}
}

void BeginRenderPassCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, fb);
	refButtonD(gui, rp);

	// area
	imGuiText("offset: {}, {}", info.renderArea.offset.x, info.renderArea.offset.y);
	imGuiText("extent: {}, {}", info.renderArea.extent.width,
		info.renderArea.extent.height);

	// clear values
	if(rp) {
		for(auto i = 0u; i < clearValues.size(); ++i) {
			dlg_assert_or(i < rp->desc.attachments.size(), break);
			auto& clearValue = clearValues[i];
			auto& att = rp->desc.attachments[i];

			if(att.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) {
				continue;
			}

			imGuiText("Attachment {} clear value:", i);
			ImGui::SameLine();

			if(FormatIsDepthOrStencil(att.format)) {
				imGuiText("Depth {}, Stencil {}",
					clearValue.depthStencil.depth,
					clearValue.depthStencil.stencil);
			} else {
				auto print = [](auto& val) {
					imGuiText("({}, {}, {}, {})", val[0], val[1], val[2], val[3]);
				};

				if(FormatIsSampledFloat(att.format)) {
					print(clearValue.color.float32);
				} else if(FormatIsSINT(att.format)) {
					print(clearValue.color.int32);
				} else if(FormatIsUINT(att.format)) {
					print(clearValue.color.uint32);
				}
			}
		}
	}

	// TODO: when using an imageless framebuffer, link to used
	// attachments here
}

bool addAllowSwapchainViews(MatchVal& m, ImageView* va, ImageView* vb) {
	if(va && vb && va != vb && va->img && vb->img && va->img->swapchain) {
		return add(m, va->img->swapchain, vb->img->swapchain);
	} else {
		// the image views have to match, not the images to account
		// for different mips or layers
		// TODO: could consider the imageView description here instead?
		// But creating similar image views for the same image is a weird corner case.
		return add(m, va, vb);
	}
}

void NextSubpassCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	// NOTE: since we always manually re-record secondary command buffers,
	// we must always pass VK_SUBPASS_CONTENTS_INLINE here.
	auto beginInfo = this->beginInfo;
	beginInfo.contents = VK_SUBPASS_CONTENTS_INLINE;

	if(this->beginInfo.pNext || this->endInfo.pNext) {
		auto f = dev.dispatch.CmdNextSubpass2;
		f(cb, &beginInfo, &this->endInfo);
	} else {
		dev.dispatch.CmdNextSubpass(cb, beginInfo.contents);
	}
}

void EndRenderPassCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(this->endInfo.pNext) {
		auto f = dev.dispatch.CmdEndRenderPass2;
		f(cb, &this->endInfo);
	} else {
		dev.dispatch.CmdEndRenderPass(cb);
	}
}

// DrawCmdBase
DrawCmdBase::DrawCmdBase() = default;

DrawCmdBase::DrawCmdBase(CommandBuffer& cb) :
		state(&cb.graphicsState()), pushConstants(cb.pushConstants()) {
}

void DrawCmdBase::displayGrahpicsState(Gui& gui, bool indices) const {
	if(indices) {
		dlg_assert(state->indices.buffer);
		imGuiText("Index Buffer: ");
		ImGui::SameLine();
		refButtonD(gui, state->indices.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}, Type {}", state->indices.offset,
			vk::name(state->indices.type));
	}

	refButtonD(gui, state->pipe);

	imGuiText("Vertex buffers");
	for(auto& vertBuf : state->vertices) {
		if(!vertBuf.buffer) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, vertBuf.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}", vertBuf.offset);
	}

	// dynamic state
	if(state->pipe && !state->pipe->dynamicState.empty()) {
		imGuiText("DynamicState");
		ImGui::Indent();

		// viewport
		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_VIEWPORT)) {
			auto count = state->pipe->viewportState.viewportCount;
			dlg_assert(state->dynamic.viewports.size() >= count);
			if(count == 1) {
				auto& vp = state->dynamic.viewports[0];
				imGuiText("Viewport: pos ({}, {}), size ({}, {}), depth [{}, {}]",
					vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
			} else if(count > 1) {
				imGuiText("Viewports");
				for(auto& vp : state->dynamic.viewports.first(count)) {
					ImGui::Bullet();
					imGuiText("pos ({}, {}), size ({}, {}), depth [{}, {}]",
						vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
				}
			}
		}
		// scissor
		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_SCISSOR)) {
			auto count = state->pipe->viewportState.scissorCount;
			dlg_assert(state->dynamic.scissors.size() >= count);
			if(count == 1) {
				auto& sc = state->dynamic.scissors[0];
				imGuiText("Scissor: offset ({}, {}), extent ({} {})",
					sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
			} else if(count > 1) {
				imGuiText("Scissors");
				for(auto& sc : state->dynamic.scissors.first(count)) {
					ImGui::Bullet();
					imGuiText("offset ({} {}), extent ({} {})",
						sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
				}
			}
		}

		// line width
		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_LINE_WIDTH)) {
			imGuiText("Line width: {}", state->dynamic.lineWidth);
		}

		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_DEPTH_BIAS)) {
			auto& db = state->dynamic.depthBias;
			imGuiText("Depth bias: constant {}, clamp {}, slope {}",
				db.constant, db.clamp, db.slope);
		}

		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
			auto& bc = state->dynamic.blendConstants;
			imGuiText("Blend Constants: {} {} {} {}",
				bc[0], bc[1], bc[2], bc[3]);
		}

		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_DEPTH_BOUNDS)) {
			imGuiText("Depth bounds: [{}, {}]",
				state->dynamic.depthBoundsMin, state->dynamic.depthBoundsMax);
		}

		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
			imGuiText("Stencil compare mask front: {}{}", std::hex,
				state->dynamic.stencilFront.compareMask);
			imGuiText("Stencil compare mask back: {}{}", std::hex,
				state->dynamic.stencilBack.compareMask);
		}

		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
			imGuiText("Stencil write mask front: {}{}", std::hex,
				state->dynamic.stencilFront.writeMask);
			imGuiText("Stencil write mask back: {}{}", std::hex,
				state->dynamic.stencilBack.writeMask);
		}

		if(state->pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
			imGuiText("Stencil reference front: {}{}", std::hex,
				state->dynamic.stencilFront.reference);
			imGuiText("Stencil reference back: {}{}", std::hex,
				state->dynamic.stencilBack.reference);
		}

		ImGui::Unindent();
	} else if(!state->pipe) {
		imGuiText("Can't display relevant dynamic state, pipeline was destroyed");
	} else if(state->pipe->dynamicState.empty()) {
		// imGuiText("No relevant dynamic state");
	}
}

bool same(const Pipeline* a, const Pipeline* b) {
	if(!a || !b) {
		return false;
	}

	if(a == b) {
		return true;
	}

	// TODO: also compare/match layout?
	if(!a->name.empty() && a->name == b->name && a->type == b->type) {
		return true;
	}

	// TODO: add option for deep matching

	return false;
}

MatchVal DrawCmdBase::doMatch(const DrawCmdBase& cmd, bool indexed) const {
	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(!same(state->pipe, cmd.state->pipe)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	m.total += 5.f;
	m.match += 5.f;

	for(auto i = 0u; i < state->pipe->vertexBindings.size(); ++i) {
		dlg_assert_or(i < state->vertices.size(), break);
		dlg_assert_or(i < cmd.state->vertices.size(), break);

		addNonNull(m, state->vertices[i].buffer, cmd.state->vertices[i].buffer);

		// Low weight on offset here, it can change frequently for dynamic
		// draw data. But the same buffer is a good indicator for similar
		// commands
		add(m, state->vertices[i].offset, cmd.state->vertices[i].offset, 0.1);
	}

	if(indexed) {
		addNonNull(m, state->indices.buffer, cmd.state->indices.buffer);
		add(m, state->indices.offset, cmd.state->indices.offset, 0.1);

		// different index types is an indicator for fundamentally different
		// commands.
		if(state->indices.type != cmd.state->indices.type) {
			return MatchVal::noMatch();
		}
	}

	for(auto& pcr : state->pipe->layout->pushConstants) {
		// TODO: these asserts can trigger if parts of the push constant
		// range was left undefined. It might not be used by the shader
		// anyways. No idea how to fix.
		dlg_assertl_or(dlg_level_warn,
			pcr.offset + pcr.size <= pushConstants.data.size(), continue);
		dlg_assertl_or(dlg_level_warn,
			pcr.offset + pcr.size <= cmd.pushConstants.data.size(), continue);

		auto pcrWeight = 1.f; // std::min(pcr.size / 4u, 4u);
		m.total += pcrWeight;
		if(std::memcmp(&pushConstants.data[pcr.offset],
				&cmd.pushConstants.data[pcr.offset], pcr.size) == 0u) {
			m.match += pcrWeight;
		}
	}

	// - we consider the bound descriptors somewhere else since they might
	//   already have been unset from the command
	// - we don't consider the render pass instance here since that should
	//   already have been taken into account via the parent commands
	// TODO: consider dynamic state?

	return m;
}

// DrawCmd
void DrawCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

std::string DrawCmd::toString() const {
	return dlg::format("Draw({}, {}, {}, {})",
		vertexCount, instanceCount, firstVertex, firstInstance);
}

void DrawCmd::displayInspector(Gui& gui) const {
	asColumns2({{
		{"vertexCount", "{}", vertexCount},
		{"instanceCount", "{}", instanceCount},
		{"firstVertex", "{}", firstVertex},
		{"firstInstance", "{}", firstInstance},
	}});

	DrawCmdBase::displayGrahpicsState(gui, false);
}

// DrawIndirectCmd
void DrawIndirectCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(indexed) {
		dev.dispatch.CmdDrawIndexedIndirect(cb, buffer->handle, offset, drawCount, stride);
	} else {
		dev.dispatch.CmdDrawIndirect(cb, buffer->handle, offset, drawCount, stride);
	}
}

void DrawIndirectCmd::displayInspector(Gui& gui) const {
	imGuiText("Indirect buffer");
	ImGui::SameLine();
	refButtonD(gui, buffer);
	ImGui::SameLine();
	imGuiText("Offset {}", offset);

	DrawCmdBase::displayGrahpicsState(gui, indexed);
}

std::string DrawIndirectCmd::toString() const {
	auto [bufNameRes, bufName] = nameRes(buffer);
	auto cmdName = indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	if(bufNameRes == NameType::named) {
		return dlg::format("{}({}, {})", cmdName, bufName, drawCount);
	} else if(drawCount > 1) {
		return dlg::format("{}(drawCount: {})", cmdName, drawCount);
	} else {
		return cmdName;
	}
}

// DrawIndexedCmd
void DrawIndexedCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

std::string DrawIndexedCmd::toString() const {
	return dlg::format("DrawIndexed({}, {}, {}, {}, {})",
		indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void DrawIndexedCmd::displayInspector(Gui& gui) const {
	asColumns2({{
		{"indexCount", "{}", indexCount},
		{"instanceCount", "{}", instanceCount},
		{"firstIndex", "{}", firstIndex},
		{"vertexOffset", "{}", vertexOffset},
		{"firstInstance", "{}", firstInstance},
	}});

	DrawCmdBase::displayGrahpicsState(gui, true);
}

// DrawIndirectCountCmd
void DrawIndirectCountCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(indexed) {
		auto f = dev.dispatch.CmdDrawIndexedIndirectCount;
		f(cb, buffer->handle, offset, countBuffer->handle, countBufferOffset,
			maxDrawCount, stride);
	} else {
		auto f = dev.dispatch.CmdDrawIndirectCount;
		f(cb, buffer->handle, offset,
			countBuffer->handle, countBufferOffset, maxDrawCount, stride);
	}
}

std::string DrawIndirectCountCmd::toString() const {
	// NOTE: we intentionally don't display any extra information here
	// since that's hard to do inuitively
	return indexed ? "DrawIndexedIndirectCount" : "DrawIndirectCount";
}

void DrawIndirectCountCmd::displayInspector(Gui& gui) const {
	imGuiText("Indirect buffer:");
	ImGui::SameLine();
	refButtonD(gui, buffer);
	ImGui::SameLine();
	imGuiText("Offset {}, Stride {}", offset, stride);

	imGuiText("Count buffer:");
	ImGui::SameLine();
	refButtonD(gui, countBuffer);
	ImGui::SameLine();
	imGuiText("Offset {}", countBufferOffset);

	DrawCmdBase::displayGrahpicsState(gui, indexed);
}

// DrawMultiCmd
std::string DrawMultiCmd::toString() const {
	return dlg::format("DrawMulti({})", vertexInfos.size());
}
void DrawMultiCmd::displayInspector(Gui& gui) const {
	// TODO
	DrawCmdBase::displayInspector(gui);
}
void DrawMultiCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	auto ourStride = sizeof(VkMultiDrawInfoEXT);
	dev.dispatch.CmdDrawMultiEXT(cb, vertexInfos.size(),
		vertexInfos.data(), instanceCount, firstInstance, ourStride);
}

// DrawMultiIndexedCmd
std::string DrawMultiIndexedCmd::toString() const {
	return dlg::format("DrawMultiIndexed({})", indexInfos.size());
}
void DrawMultiIndexedCmd::displayInspector(Gui& gui) const {
	// TODO
	DrawCmdBase::displayInspector(gui);
}
void DrawMultiIndexedCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	auto ourStride = sizeof(VkMultiDrawIndexedInfoEXT);
	dev.dispatch.CmdDrawMultiIndexedEXT(cb, indexInfos.size(),
		indexInfos.data(), instanceCount, firstInstance, ourStride,
		vertexOffset ? &*vertexOffset : nullptr);
}

// BindVertexBuffersCmd
void BindVertexBuffersCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	std::vector<VkBuffer> vkbuffers;
	std::vector<VkDeviceSize> vkoffsets;
	vkbuffers.reserve(buffers.size());
	vkoffsets.reserve(buffers.size());
	for(auto& b : buffers) {
		vkbuffers.push_back(b.buffer->handle);
		vkoffsets.push_back(b.offset);
	}

	dev.dispatch.CmdBindVertexBuffers(cb, firstBinding,
		u32(vkbuffers.size()), vkbuffers.data(), vkoffsets.data());
}

std::string BindVertexBuffersCmd::toString() const {
	if(buffers.size() == 1) {
		auto [buf0NameRes, buf0Name] = nameRes(buffers[0].buffer);
		if(buf0NameRes == NameType::named) {
			return dlg::format("BindVertexBuffer({}: {})", firstBinding, buf0Name);
		} else {
			return dlg::format("BindVertexBuffer({})", firstBinding);
		}
	} else {
		return dlg::format("BindVertexBuffers({}..{})", firstBinding,
			firstBinding + buffers.size() - 1);
	}
}

void BindVertexBuffersCmd::displayInspector(Gui& gui) const {
	for(auto i = 0u; i < buffers.size(); ++i) {
		ImGui::Bullet();
		imGuiText("{}: ", firstBinding + i);
		ImGui::SameLine();
		refButtonD(gui, buffers[i].buffer);
	}
}

// BindIndexBufferCmd
void BindIndexBufferCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdBindIndexBuffer(cb, buffer->handle, offset, indexType);
}

// BindDescriptorSetCmd
void BindDescriptorSetCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	ThreadMemScope memScope;
	auto vkds = rawHandles(memScope, sets);
	dev.dispatch.CmdBindDescriptorSets(cb, pipeBindPoint, pipeLayout->handle,
		firstSet, u32(vkds.size()), vkds.data(),
		u32(dynamicOffsets.size()), dynamicOffsets.data());
}

std::string BindDescriptorSetCmd::toString() const {
	if(sets.size() == 1) {
		// NOTE: we can't rely on the set handles being valid anymore
		// Could make it an env var option.
		// auto [ds0Res, ds0Name] = name(sets[0]);
		// if(ds0Res == NameType::named) {
		// 	return dlg::format("BindDescriptorSet({}: {})", firstSet, ds0Name);
		// } else {
		// 	return dlg::format("BindDescriptorSet({})", firstSet);
		// }
		return dlg::format("BindDescriptorSet({})", firstSet);
	} else {
		return dlg::format("BindDescriptorSets({}..{})",
			firstSet, firstSet + sets.size() - 1);
	}
}

void BindDescriptorSetCmd::displayInspector(Gui& gui) const {
	imGuiText("Bind point: {}", vk::name(pipeBindPoint));
	imGuiText("First set: {}", firstSet);

	refButtonD(gui, pipeLayout);

	// TODO: display dynamic offsets

	// NOTE: we can't rely on the set handles being valid anymore.
	// Could make it an env var option.
	/*
	for (auto* ds : sets) {
		ImGui::Bullet();

		if(!ds) {
			imGuiText("null or invalidated");
		} else {
			refButton(gui, *ds);
		}
	}
	*/
}

// DispatchCmdBase
DispatchCmdBase::DispatchCmdBase(CommandBuffer& cb) :
		state(&cb.computeState()), pushConstants(cb.pushConstants()) {
}

void DispatchCmdBase::displayComputeState(Gui& gui) const {
	refButtonD(gui, state->pipe);
}

MatchVal DispatchCmdBase::doMatch(const DispatchCmdBase& cmd) const {
	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(!same(state->pipe, cmd.state->pipe)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	for(auto& pcr : state->pipe->layout->pushConstants) {
		dlg_assert_or(pcr.offset + pcr.size <= pushConstants.data.size(), continue);
		dlg_assert_or(pcr.offset + pcr.size <= cmd.pushConstants.data.size(), continue);

		m.total += pcr.size;
		if(std::memcmp(&pushConstants.data[pcr.offset],
				&cmd.pushConstants.data[pcr.offset], pcr.size) == 0u) {
			m.match += pcr.size;
		}
	}

	// - we consider the bound descriptors somewhere else since they might
	//   already have been unset from the command

	return m;
}

// DispatchCmd
void DispatchCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdDispatch(cb, groupsX, groupsY, groupsZ);
}

std::string DispatchCmd::toString() const {
	return dlg::format("Dispatch({}, {}, {})", groupsX, groupsY, groupsZ);
}

void DispatchCmd::displayInspector(Gui& gui) const {
	imGuiText("groups X: {}", groupsX);
	imGuiText("groups Y: {}", groupsY);
	imGuiText("groups Z: {}", groupsZ);
	DispatchCmdBase::displayComputeState(gui);
}

// DispatchIndirectCmd
void DispatchIndirectCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdDispatchIndirect(cb, buffer->handle, offset);
}

void DispatchIndirectCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, buffer);
	DispatchCmdBase::displayComputeState(gui);
}

std::string DispatchIndirectCmd::toString() const {
	auto [bufNameRes, bufName] = nameRes(buffer);
	if(bufNameRes == NameType::named) {
		return dlg::format("DispatchIndirect({})", bufName);
	}

	return "DispatchIndirect";
}

// DispatchBaseCmd
void DispatchBaseCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	auto f = dev.dispatch.CmdDispatchBase;
	f(cb, baseGroupX, baseGroupY, baseGroupZ, groupsX, groupsY, groupsZ);
}

std::string DispatchBaseCmd::toString() const {
	return dlg::format("DispatchBase({}, {}, {}, {}, {}, {})",
		baseGroupX, baseGroupY, baseGroupZ, groupsX, groupsY, groupsZ);
}

void DispatchBaseCmd::displayInspector(Gui& gui) const {
	imGuiText("Base: {} {} {}", baseGroupX, baseGroupY, baseGroupZ);
	imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
	DispatchCmdBase::displayComputeState(gui);
}

// CopyImageCmd
void CopyImageCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(dev.dispatch.CmdCopyImage2KHR) {
		VkCopyImageInfo2KHR info = {
			VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2_KHR,
			pNext,
			src->handle,
			srcLayout,
			dst->handle,
			dstLayout,
			u32(copies.size()),
			copies.data(),
		};

		dev.dispatch.CmdCopyImage2KHR(cb, &info);
	} else {
		dlg_assert(!pNext);
		auto copiesD = downgrade<VkImageCopy>(copies);
		dev.dispatch.CmdCopyImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
			u32(copiesD.size()), copiesD.data());
	}
}

std::string CopyImageCmd::toString() const {
	auto [srcRes, srcName] = nameRes(src);
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyImage({} -> {})", srcName, dstName);
	} else {
		return "CopyImage";
	}
}

void CopyImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Copies");

	for(auto& copy : copies) {
		auto srcRegion = printImageRegion(src, copy.srcOffset, copy.srcSubresource);
		auto dstRegion = printImageRegion(dst, copy.dstOffset, copy.dstSubresource);

		std::string sizeString;
		if(src && dst && src->ci.imageType == VK_IMAGE_TYPE_1D && dst->ci.imageType == VK_IMAGE_TYPE_1D) {
			sizeString = dlg::format("{}", copy.extent.width);
		} else if(src && dst && src->ci.imageType <= VK_IMAGE_TYPE_2D && dst->ci.imageType <= VK_IMAGE_TYPE_2D) {
			sizeString = dlg::format("{} x {}", copy.extent.width, copy.extent.height);
		} else {
			sizeString = dlg::format("{} x {} x {}", copy.extent.width, copy.extent.height, copy.extent.depth);
		}

		ImGui::Bullet();
		imGuiText("{} -> {} [{}]", srcRegion, dstRegion, sizeString);
	}
}

// CopyBufferToImageCmd
void CopyBufferToImageCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(dev.dispatch.CmdCopyBufferToImage2KHR) {
		VkCopyBufferToImageInfo2KHR info = {
			VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2_KHR,
			pNext,
			src->handle,
			dst->handle,
			dstLayout,
			u32(copies.size()),
			copies.data(),
		};

		dev.dispatch.CmdCopyBufferToImage2KHR(cb, &info);
	} else {
		dlg_assert(!pNext);
		auto copiesD = downgrade<VkBufferImageCopy>(copies);
		dev.dispatch.CmdCopyBufferToImage(cb, src->handle, dst->handle,
			dstLayout, u32(copiesD.size()), copiesD.data());
	}
}

void CopyBufferToImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Copies");

	for(auto& copy : copies) {
		ImGui::Bullet();
		imGuiText("{}", printBufferImageCopy(dst, copy, true));
	}
}

std::string CopyBufferToImageCmd::toString() const {
	auto [srcRes, srcName] = nameRes(src);
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyBufferToImage({} -> {})", srcName, dstName);
	} else {
		return "CopyBufferToImage";
	}
}

// CopyImageToBufferCmd
void CopyImageToBufferCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(dev.dispatch.CmdCopyImageToBuffer2KHR) {
		VkCopyImageToBufferInfo2KHR info = {
			VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2_KHR,
			pNext,
			src->handle,
			srcLayout,
			dst->handle,
			u32(copies.size()),
			copies.data(),
		};

		dev.dispatch.CmdCopyImageToBuffer2KHR(cb, &info);
	} else {
		dlg_assert(!pNext);
		auto copiesD = downgrade<VkBufferImageCopy>(copies);
		dev.dispatch.CmdCopyImageToBuffer(cb, src->handle, srcLayout, dst->handle,
			u32(copiesD.size()), copiesD.data());
	}
}

void CopyImageToBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Copies");

	for(auto& copy : copies) {
		ImGui::Bullet();
		imGuiText("{}", printBufferImageCopy(src, copy, false));
	}
}

std::string CopyImageToBufferCmd::toString() const {
	auto [srcRes, srcName] = nameRes(src);
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyImageToBuffer({} -> {})", srcName, dstName);
	} else {
		return "CopyImageToBuffer";
	}
}

// BlitImageCmd
void BlitImageCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(dev.dispatch.CmdBlitImage2KHR) {
		VkBlitImageInfo2KHR info = {
			VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2_KHR,
			pNext,
			src->handle,
			srcLayout,
			dst->handle,
			dstLayout,
			u32(blits.size()),
			blits.data(),
			filter,
		};

		dev.dispatch.CmdBlitImage2KHR(cb, &info);
	} else {
		dlg_assert(!pNext);
		auto blitsD = downgrade<VkImageBlit>(blits);
		dev.dispatch.CmdBlitImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
			u32(blitsD.size()), blitsD.data(), filter);
	}
}

void BlitImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	imGuiText("Filter {}", vk::name(filter));

	ImGui::Spacing();
	imGuiText("Blits");

	for(auto& blit : blits) {
		auto srcSubres = printImageSubresLayers(src, blit.srcSubresource);
		auto src0 = printImageOffset(src, blit.srcOffsets[0]);
		auto src1 = printImageOffset(src, blit.srcOffsets[1]);

		auto dstSubres = printImageSubresLayers(dst, blit.dstSubresource);
		auto dst0 = printImageOffset(dst, blit.dstOffsets[0]);
		auto dst1 = printImageOffset(dst, blit.dstOffsets[1]);

		auto srcSep = srcSubres.empty() ? "" : ": ";
		auto dstSep = dstSubres.empty() ? "" : ": ";

		ImGui::Bullet();
		imGuiText("({}{}({})..({}) -> ({}{}({})..({}))",
			srcSubres, srcSep, src0, src1,
			dstSubres, dstSep, dst0, dst1);
	}
}

std::string BlitImageCmd::toString() const {
	auto [srcRes, srcName] = nameRes(src);
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("BlitImage({} -> {})", srcName, dstName);
	} else {
		return "BlitImage";
	}
}

// ResolveImageCmd
void ResolveImageCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(dev.dispatch.CmdResolveImage2KHR) {
		VkResolveImageInfo2KHR info = {
			VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2_KHR,
			pNext,
			src->handle,
			srcLayout,
			dst->handle,
			dstLayout,
			u32(regions.size()),
			regions.data(),
		};

		dev.dispatch.CmdResolveImage2KHR(cb, &info);
	} else {
		dlg_assert(!pNext);
		auto regionsD = downgrade<VkImageResolve>(regions);
		dev.dispatch.CmdResolveImage(cb, src->handle, srcLayout,
			dst->handle, dstLayout, u32(regionsD.size()), regionsD.data());
	}
}

void ResolveImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Regions");

	// Basically same as CopyImageCmd
	for(auto& copy : regions) {
		auto srcRegion = printImageRegion(src, copy.srcOffset, copy.srcSubresource);
		auto dstRegion = printImageRegion(dst, copy.dstOffset, copy.dstSubresource);

		std::string sizeString;
		if(src && dst && src->ci.imageType == VK_IMAGE_TYPE_1D && dst->ci.imageType == VK_IMAGE_TYPE_1D) {
			sizeString = dlg::format("{}", copy.extent.width);
		} else if(src && dst && src->ci.imageType <= VK_IMAGE_TYPE_2D && dst->ci.imageType <= VK_IMAGE_TYPE_2D) {
			sizeString = dlg::format("{} x {}", copy.extent.width, copy.extent.height);
		} else {
			sizeString = dlg::format("{} x {} x {}", copy.extent.width, copy.extent.height, copy.extent.depth);
		}

		ImGui::Bullet();
		imGuiText("{} -> {} [{}]", srcRegion, dstRegion, sizeString);
	}
}

std::string ResolveImageCmd::toString() const {
	auto [srcRes, srcName] = nameRes(src);
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("ResolveImage({} -> {})", srcName, dstName);
	} else {
		return "ResolveImage";
	}
}

// CopyBufferCmd
void CopyBufferCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(dev.dispatch.CmdCopyBuffer2KHR) {
		VkCopyBufferInfo2KHR info = {
			VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2_KHR,
			pNext,
			src->handle,
			dst->handle,
			u32(regions.size()),
			regions.data(),
		};

		dev.dispatch.CmdCopyBuffer2KHR(cb, &info);
	} else {
		dlg_assert(!pNext);
		auto regionsD = downgrade<VkBufferCopy>(regions);
		dev.dispatch.CmdCopyBuffer(cb, src->handle, dst->handle,
			u32(regionsD.size()), regionsD.data());
	}
}

void CopyBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	ImGui::Spacing();
	imGuiText("Regions");

	for(auto& region : regions) {
		ImGui::Bullet();
		imGuiText("offsets {} -> {}, size {}", region.srcOffset, region.dstOffset, region.size);
	}
}

std::string CopyBufferCmd::toString() const {
	auto [srcRes, srcName] = nameRes(src);
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyBuffer({} -> {})", srcName, dstName);
	} else {
		return "CopyBuffer";
	}
}

// UpdateBufferCmd
void UpdateBufferCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdUpdateBuffer(cb, dst->handle, offset, data.size(), data.data());
}

std::string UpdateBufferCmd::toString() const {
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named) {
		return dlg::format("UpdateBuffer({})", dstName);
	} else {
		return "UpdateBuffer";
	}
}

void UpdateBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	ImGui::SameLine();
	imGuiText("Offset {}", offset);

	// TODO: display data?
}

// FillBufferCmd
void FillBufferCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdFillBuffer(cb, dst->handle, offset, size, data);
}

std::string FillBufferCmd::toString() const {
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named) {
		return dlg::format("FillBuffer({})", dstName);
	} else {
		return "FillBuffer";
	}
}

void FillBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	ImGui::SameLine();
	imGuiText("Offset {}, Size {}", offset, size);

	imGuiText("Filled with {}{}", std::hex, data);
}

// ClearColorImageCmd
void ClearColorImageCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdClearColorImage(cb, dst->handle, dstLayout, &color,
		u32(ranges.size()), ranges.data());
}

std::string ClearColorImageCmd::toString() const {
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named) {
		return dlg::format("ClearColorImage({})", dstName);
	} else {
		return "ClearColorImage";
	}
}

void ClearColorImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	// TODO: color, layout, ranges
}

// ClearDepthStencilImageCmd
void ClearDepthStencilImageCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdClearDepthStencilImage(cb, dst->handle, dstLayout, &value,
		u32(ranges.size()), ranges.data());
}

std::string ClearDepthStencilImageCmd::toString() const {
	auto [dstRes, dstName] = nameRes(dst);
	if(dstRes == NameType::named) {
		return dlg::format("ClearDepthStencilImage({})", dstName);
	} else {
		return "ClearColorImage";
	}
}

void ClearDepthStencilImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, dst);
	// TODO: value, layout, ranges
}

// Clear AttachhmentCmd
void ClearAttachmentCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdClearAttachments(cb, u32(attachments.size()),
		attachments.data(), u32(rects.size()), rects.data());
}

void ClearAttachmentCmd::displayInspector(Gui& gui) const {
	// TODO: we probably need to refer to used render pass/fb here
	(void) gui;
}

// SetEventCmd
void SetEventCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetEvent(cb, event->handle, stageMask);
}

std::string SetEventCmd::toString() const {
	auto [nameRes, eventName] = vil::nameRes(event);
	if(nameRes == NameType::named) {
		return dlg::format("SetEvent({})", eventName);
	}

	return "SetEvent";
}

void SetEventCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, event);
	imGuiText("Stages: {}", vk::namePipelineStageFlags(stageMask));
}

// SetEvent2Cmd
void SetEvent2Cmd::record(const Device& dev, VkCommandBuffer cb, u32 qfam) const {
	ThreadMemScope tms;
	auto depInfo = patchedBarriers(tms, qfam);
	dev.dispatch.CmdSetEvent2(cb, event->handle, &depInfo);
}

std::string SetEvent2Cmd::toString() const {
	auto [nameRes, eventName] = vil::nameRes(event);
	if(nameRes == NameType::named) {
		return dlg::format("SetEvent2({})", eventName);
	}

	return "SetEvent2";
}

void SetEvent2Cmd::displayInspector(Gui& gui) const {
	refButtonD(gui, event);
	Barrier2CmdBase::displayInspector(gui);
}

// ResetEventCmd
void ResetEventCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(legacy) {
		dev.dispatch.CmdResetEvent(cb, event->handle,
			downgradePipelineStageFlags(stageMask));
	} else {
		dlg_assert(dev.dispatch.CmdResetEvent2);
		dev.dispatch.CmdResetEvent2(cb, event->handle, stageMask);
	}
}

std::string ResetEventCmd::toString() const {
	auto ownName = nameDesc();
	auto [nameRes, eventName] = vil::nameRes(event);
	if(nameRes == NameType::named) {
		return dlg::format("{}({})", ownName, eventName);
	}

	return std::string(ownName);
}

void ResetEventCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, event);
	imGuiText("Stages: {}", vk::namePipelineStageFlags2(stageMask));
}

// ExecuteCommandsCmd
void ExecuteCommandsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	// NOTE: we don't do anything here. When re-recording we always want to
	// see/potentially hook all commands and therefore will manually record
	// the secondary command records (children of this command) as well.
	(void) dev;
	(void) cb;

	/*
	std::vector<VkCommandBuffer> vkcbs;
	auto child = children_;
	while(child) {
		auto* echild = commandCast<ExecuteCommandsChildCmd*>(child);
		dlg_assert(echild);
		dlg_assert(echild->record_->cb);
		vkcbs.push_back(echild->record_->cb->handle());

		child = child->next;
	}

	dev.dispatch.CmdExecuteCommands(cb, u32(vkcbs.size()), vkcbs.data());
	*/
}

void ExecuteCommandsCmd::displayInspector(Gui& gui) const {
	auto echild = commandCast<ExecuteCommandsChildCmd*>(children_);
	while(echild) {
		// TODO: could link to command buffer (if still valid/linked)
		auto label = dlg::format("View Recording {}", echild->id_);
		if(ImGui::Button(label.c_str())) {
			// We can convert raw pointer into IntrusivePtr here since
			// we know that it's still alive; it's kept alive by our
			// parent CommandRecord (secondaries)
			gui.cbGui().select(IntrusivePtr<CommandRecord>(echild->record_));
			gui.activateTab(Gui::Tab::commandBuffer);
		}

		echild = commandCast<ExecuteCommandsChildCmd*>(echild->next);
	}
}

// ExecuteCommandsChildCmd
std::string ExecuteCommandsChildCmd::toString() const {
	auto [cbRes, cbName] = nameRes(record_->cb);
	if(cbRes == NameType::named) {
		return dlg::format("{}: {}", id_, cbName);
	} else {
		return dlg::format("{}", id_);
	}
}

const ParentCommand::SectionStats& ExecuteCommandsChildCmd::sectionStats() const {
	return record_->commands->stats_;
}

Command* ExecuteCommandsChildCmd::children() const {
	return record_->commands->children_;
}

ParentCommand* ExecuteCommandsChildCmd::firstChildParent() const {
	return record_->commands->firstChildParent_;
}

// BeginDebugUtilsLabelCmd
void BeginDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	VkDebugUtilsLabelEXT label {};
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = this->name;
	std::memcpy(&label.color, this->color.data(), sizeof(label.color));
	dev.dispatch.CmdBeginDebugUtilsLabelEXT(cb, &label);
}

// EndDebugUtilsLabelCmd
void EndDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdEndDebugUtilsLabelEXT(cb);
}

// InsertDebugUtilsLabelCmd
void InsertDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	VkDebugUtilsLabelEXT label {};
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = this->name;
	std::memcpy(&label.color, this->color.data(), sizeof(label.color));
	dev.dispatch.CmdInsertDebugUtilsLabelEXT(cb, &label);
}

// BindPipelineCmd
void BindPipelineCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdBindPipeline(cb, bindPoint, pipe->handle);
}

void BindPipelineCmd::displayInspector(Gui& gui) const {
	dlg_assert(pipe->type == bindPoint);
	if(bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		refButtonD(gui, static_cast<ComputePipeline*>(pipe));
	} else if(bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		refButtonD(gui, static_cast<GraphicsPipeline*>(pipe));
	}
}

std::string BindPipelineCmd::toString() const {
	auto bp = (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) ? "compute" : "graphics";
	auto [nameRes, pipeName] = vil::nameRes(pipe);
	if(nameRes == NameType::named) {
		return dlg::format("BindPipeline({}: {})", bp, pipeName);
	} else {
		return dlg::format("BindPipeline({})", bp);
	}
}

// PushConstantsCmd
void PushConstantsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdPushConstants(cb, pipeLayout->handle, stages, offset,
		u32(values.size()), values.data());
}

// SetViewportCmd
void SetViewportCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetViewport(cb, first, u32(viewports.size()), viewports.data());
}

void SetViewportCmd::displayInspector(Gui&) const {
	imGuiText("First: {}", first);
	imGuiText("Viewports:");

	for(auto& viewport : viewports) {
		ImGui::Bullet();
		imGuiText("pos: ({}, {}), size: ({}, {}), depth: {} - {}",
			viewport.x, viewport.y, viewport.width, viewport.height,
			viewport.minDepth, viewport.maxDepth);
	}
}

// SetScissorCmd
void SetScissorCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetScissor(cb, first, u32(scissors.size()), scissors.data());
}

void SetScissorCmd::displayInspector(Gui&) const {
	imGuiText("First: {}", first);
	imGuiText("Scissors:");

	for(auto& scissor : scissors) {
		ImGui::Bullet();
		imGuiText("pos: ({}, {}), size: ({}, {})",
			scissor.offset.x, scissor.offset.y,
			scissor.extent.width, scissor.extent.height);
	}
}

// SetLineWidthCmd
void SetLineWidthCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetLineWidth(cb, width);
}

void SetLineWidthCmd::displayInspector(Gui&) const {
	imGuiText("width: {}", width);
}

// other cmds
void SetDepthBiasCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthBias(cb, state.constant, state.clamp, state.slope);
}

void SetBlendConstantsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetBlendConstants(cb, values.data());
}

void SetStencilCompareMaskCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetStencilCompareMask(cb, faceMask, value);
}

void SetStencilWriteMaskCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetStencilWriteMask(cb, faceMask, value);
}

void SetStencilReferenceCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetStencilReference(cb, faceMask, value);
}

void SetDepthBoundsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthBounds(cb, min, max);
}

// BeginQuery
void BeginQueryCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdBeginQuery(cb, pool->handle, query, flags);
}

// EndQuery
void EndQueryCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdEndQuery(cb, pool->handle, query);
}

// ResetQuery
void ResetQueryPoolCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdResetQueryPool(cb, pool->handle, first, count);
}

// WriteTimestamp
void WriteTimestampCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	if(legacy) {
		dev.dispatch.CmdWriteTimestamp(cb, downgradePipelineStageBits(stage),
			pool->handle, query);
	} else {
		dlg_assert(dev.dispatch.CmdWriteTimestamp2);
		dev.dispatch.CmdWriteTimestamp2(cb, stage, pool->handle, query);
	}
}

// CopyQueryPool
void CopyQueryPoolResultsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdCopyQueryPoolResults(cb, pool->handle, first, count,
		dstBuffer->handle, dstOffset, stride, flags);
}

// PushDescriptorSet
void PushDescriptorSetCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdPushDescriptorSetKHR(cb, bindPoint, pipeLayout->handle,
		set, u32(descriptorWrites.size()), descriptorWrites.data());
}

// PushDescriptorSetWithTemplate
void PushDescriptorSetWithTemplateCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdPushDescriptorSetWithTemplateKHR(cb, updateTemplate->handle,
		pipeLayout->handle, set, static_cast<const void*>(data.data()));
}

// VK_KHR_fragment_shading_rate
void SetFragmentShadingRateCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetFragmentShadingRateKHR(cb, &fragmentSize, combinerOps.data());
}

// Conditional rendering
// TODO: match for BeginConditionalRenderingCmd!

void BeginConditionalRenderingCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dlg_assert(buffer);

	VkConditionalRenderingBeginInfoEXT info {};
	info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
	info.buffer = buffer->handle;
	info.offset = offset;
	info.flags = flags;

	dev.dispatch.CmdBeginConditionalRenderingEXT(cb, &info);
}

void EndConditionalRenderingCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdEndConditionalRenderingEXT(cb);
}

// VK_EXT_line_rasterization
void SetLineStippleCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetLineStippleEXT(cb, stippleFactor, stipplePattern);
}

// VK_EXT_extended_dynamic_state
void SetCullModeCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetCullModeEXT(cb, cullMode);
}

void SetFrontFaceCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetFrontFaceEXT(cb, frontFace);
}

void SetPrimitiveTopologyCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetPrimitiveTopologyEXT(cb, topology);
}

void SetViewportWithCountCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetViewportWithCountEXT(cb, viewports.size(), viewports.data());
}

void SetScissorWithCountCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetScissorWithCountEXT(cb, scissors.size(), scissors.data());
}

void SetDepthTestEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthTestEnableEXT(cb, enable);
}

void SetDepthWriteEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthWriteEnableEXT(cb, enable);
}

void SetDepthCompareOpCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthBoundsTestEnableEXT(cb, op);
}

void SetDepthBoundsTestEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthBoundsTestEnableEXT(cb, enable);
}

void SetStencilTestEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetStencilTestEnableEXT(cb, enable);
}

void SetStencilOpCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetStencilOpEXT(cb, faceMask, failOp, passOp,
		depthFailOp, compareOp);
}

// VK_EXT_extended_dynamic_state2
void SetPatchControlPointsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetPatchControlPointsEXT(cb, this->patchControlPoints);
}

void SetRasterizerDiscardEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetRasterizerDiscardEnableEXT(cb, this->enable);
}

void SetDepthBiasEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDepthBiasEnableEXT(cb, this->enable);
}

void SetLogicOpCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetLogicOpEXT(cb, this->logicOp);
}

void SetPrimitiveRestartEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetPrimitiveRestartEnableEXT(cb, this->enable);
}

// ---
void SetSampleLocationsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetSampleLocationsEXT(cb, &this->info);
}

void SetDiscardRectangleCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetDiscardRectangleEXT(cb, first, rects.size(), rects.data());
}

// VK_KHR_acceleration_structure
void CopyAccelStructCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	VkCopyAccelerationStructureInfoKHR info {};
	info.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
	info.pNext = pNext;
	info.src = src->handle;
	info.dst = dst->handle;
	info.mode = mode;
	dev.dispatch.CmdCopyAccelerationStructureKHR(cb, &info);
}

void CopyAccelStructToMemoryCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	VkCopyAccelerationStructureToMemoryInfoKHR info {};
	info.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR;
	info.pNext = pNext;
	info.src = src->handle;
	info.dst = dst;
	info.mode = mode;
	dev.dispatch.CmdCopyAccelerationStructureToMemoryKHR(cb, &info);
}

void CopyMemoryToAccelStructCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	VkCopyMemoryToAccelerationStructureInfoKHR info {};
	info.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR;
	info.pNext = pNext;
	info.src = src;
	info.dst = dst->handle;
	info.mode = mode;
	dev.dispatch.CmdCopyMemoryToAccelerationStructureKHR(cb, &info);
}

void WriteAccelStructsPropertiesCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	ThreadMemScope memScope;
	auto vkAccelStructs = rawHandles(memScope, accelStructs);

	dev.dispatch.CmdWriteAccelerationStructuresPropertiesKHR(cb,
		u32(vkAccelStructs.size()), vkAccelStructs.data(), queryType,
		queryPool->handle, firstQuery);
}

// BuildAccelStructs
void BuildAccelStructsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dlg_assert(buildInfos.size() == buildRangeInfos.size());

	ThreadMemScope memScope;
	auto ppRangeInfos = memScope.alloc<VkAccelerationStructureBuildRangeInfoKHR*>(buildRangeInfos.size());
	for(auto i = 0u; i < buildRangeInfos.size(); ++i) {
		ppRangeInfos[i] = buildRangeInfos[i].data();
	}

	dev.dispatch.CmdBuildAccelerationStructuresKHR(cb, u32(buildInfos.size()),
		buildInfos.data(), ppRangeInfos.data());
}

void BuildAccelStructsIndirectCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdBuildAccelerationStructuresIndirectKHR(cb, u32(buildInfos.size()),
		buildInfos.data(), indirectAddresses.data(), indirectStrides.data(),
		maxPrimitiveCounts.data());
}

// VK_KHR_ray_tracing_pipeline
TraceRaysCmdBase::TraceRaysCmdBase(CommandBuffer& cb) :
		state(&cb.rayTracingState()), pushConstants(cb.pushConstants()) {
}

MatchVal TraceRaysCmdBase::doMatch(const TraceRaysCmdBase& cmd) const {
	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(!same(state->pipe, cmd.state->pipe)) {
		return MatchVal::noMatch();
	}

	MatchVal m;
	for(auto& pcr : state->pipe->layout->pushConstants) {
		dlg_assert_or(pcr.offset + pcr.size <= pushConstants.data.size(), continue);
		dlg_assert_or(pcr.offset + pcr.size <= cmd.pushConstants.data.size(), continue);

		m.total += pcr.size;
		if(std::memcmp(&pushConstants.data[pcr.offset],
				&cmd.pushConstants.data[pcr.offset], pcr.size) == 0u) {
			m.match += pcr.size;
		}
	}

	// - we consider the bound descriptors somewhere else since they might
	//   already have been unset from the command

	return m;
}

void TraceRaysCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdTraceRaysKHR(cb,
		&raygenBindingTable, &missBindingTable, &hitBindingTable, &callableBindingTable,
		width, height, depth);
}

std::string TraceRaysCmd::toString() const {
	return dlg::format("TraceRays({}, {}, {})", width, height, depth);
}

void TraceRaysIndirectCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdTraceRaysIndirectKHR(cb,
		&raygenBindingTable, &missBindingTable, &hitBindingTable, &callableBindingTable,
		indirectDeviceAddress);
}

void SetRayTracingPipelineStackSizeCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetRayTracingPipelineStackSizeKHR(cb, stackSize);
}

void BeginRenderingCmd::record(const Device& dev, VkCommandBuffer cb,
		bool skipResolves,
		std::optional<VkAttachmentLoadOp> overrideLoad,
		std::optional<VkAttachmentStoreOp> overrideStore) const {
	ThreadMemScope ms;

	auto convert = [&](const Attachment& src, VkRenderingAttachmentInfo& dst) {
		dst = {};

		dst.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		dst.imageLayout = src.imageLayout;
		dst.resolveImageLayout = src.resolveImageLayout;
		dst.resolveMode = src.resolveMode;
		dst.clearValue = src.clearValue;
		dst.loadOp = overrideLoad ? *overrideLoad : src.loadOp;
		dst.storeOp = overrideStore ? *overrideStore : src.storeOp;

		if(src.view) {
			dst.imageView = src.view->handle;
		}

		if(src.resolveView && !skipResolves) {
			dst.resolveImageView = src.resolveView->handle;
		}
	};

	auto colorAtts = ms.alloc<VkRenderingAttachmentInfo>(colorAttachments.size());
	for(auto [i, att] : enumerate(colorAttachments)) {
		convert(att, colorAtts[i]);
	}

	VkRenderingInfo info {};
	info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	info.colorAttachmentCount = u32(colorAtts.size());
	info.pColorAttachments = colorAtts.data();

	VkRenderingAttachmentInfo dstDepth;
	if(depthAttachment.view) {
		convert(depthAttachment, dstDepth);
		info.pDepthAttachment = &dstDepth;
	}

	VkRenderingAttachmentInfo dstStencil;
	if(stencilAttachment.view) {
		convert(stencilAttachment, dstStencil);
		info.pStencilAttachment = &dstStencil;
	}

	info.layerCount = this->layerCount;
	info.renderArea = this->renderArea;
	info.viewMask = this->viewMask;
	info.flags = this->flags;

	dlg_assert(dev.dispatch.CmdBeginRendering);
	dev.dispatch.CmdBeginRendering(cb, &info);
}

void BeginRenderingCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	this->record(dev, cb, false, std::nullopt, std::nullopt);
}

const BeginRenderingCmd::Attachment* BeginRenderingCmd::findAttachment(const Image& img) const {
	// UGLY
	assertOwned(img.dev->mutex);

	if(depthAttachment.view && depthAttachment.view->img == &img) {
		return &depthAttachment;
	}
	if(stencilAttachment.view && stencilAttachment.view->img == &img) {
		return &stencilAttachment;
	}
	for(auto& colorAtt : colorAttachments) {
		if(colorAtt.view->img == &img) {
			return &colorAtt;
		}
	}

	return nullptr;
}

void EndRenderingCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dlg_assert(dev.dispatch.CmdEndRendering);
	dev.dispatch.CmdEndRendering(cb);
}

void SetVertexInputCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dlg_assert(dev.dispatch.CmdSetVertexInputEXT);
	dev.dispatch.CmdSetVertexInputEXT(cb,
		u32(bindings.size()), bindings.data(),
		u32(attribs.size()), attribs.data());
}

void SetColorWriteEnableCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dlg_assert(dev.dispatch.CmdSetColorWriteEnableEXT);
	dev.dispatch.CmdSetColorWriteEnableEXT(cb,
		u32(writeEnables.size()), writeEnables.data());
}

bool isIndirect(const Command& cmd) {
	return
		cmd.type() == CommandType::drawIndirect ||
		cmd.type() == CommandType::drawIndirectCount ||
		cmd.type() == CommandType::dispatchIndirect ||
		cmd.type() == CommandType::traceRaysIndirect;
}

} // namespace vil
