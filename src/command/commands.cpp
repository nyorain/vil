#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <command/match.hpp>
#include <handles.hpp>
#include <shader.hpp>
#include <cb.hpp>
#include <accelStruct.hpp>
#include <threadContext.hpp>
#include <util/span.hpp>
#include <util/util.hpp>
#include <util/ext.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/cb.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>
#include <vk/format_utils.h>
#include <iomanip>
#include <filesystem>

// TODO:
// - a lot of commands are still missing valid match() implementations.
//   some commands (bind, sync) will need contextual information, i.e.
//   an external match implementation. Or maybe just having 'prev'
//   links in addition to 'next' is already enough? probably not,
//   the commands itself also should not do the iteration, should not
//   know about other commands.

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

Matcher Command::match(const Command& cmd) const {
	// default implementation: when the types are the same, return 1,
	// otherwise 0
	if(typeid(cmd) != typeid(*this)) {
		return Matcher::noMatch();
	}

	return {1.f, 1.f};
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


bool operator==(const VkImageSubresourceLayers& a, const VkImageSubresourceLayers& b) {
	return a.aspectMask == b.aspectMask &&
		a.baseArrayLayer == b.baseArrayLayer &&
		a.layerCount == b.layerCount &&
		a.mipLevel == b.mipLevel;
}

Matcher match(const VkBufferCopy2KHR& a, const VkBufferCopy2KHR& b) {
	Matcher m;
	add(m, a.size, b.size);
	add(m, a.srcOffset, b.srcOffset, 0.2);
	add(m, a.dstOffset, b.dstOffset, 0.2);
	return m;
}

Matcher match(const VkImageCopy2KHR& a, const VkImageCopy2KHR& b) {
	Matcher m;
	addMemcmp(m, a.dstOffset, b.dstOffset);
	addMemcmp(m, a.srcOffset, b.srcOffset);
	addMemcmp(m, a.extent, b.extent);
	add(m, a.dstSubresource, b.dstSubresource);
	add(m, a.srcSubresource, b.srcSubresource);
	return m;
}

Matcher match(const VkImageBlit2KHR& a, const VkImageBlit2KHR& b) {
	Matcher m;
	add(m, a.srcSubresource, b.srcSubresource);
	add(m, a.dstSubresource, b.dstSubresource);
	addMemcmp(m, a.srcOffsets, b.srcOffsets);
	addMemcmp(m, a.dstOffsets, b.dstOffsets);
	return m;
}

Matcher match(const VkImageResolve2KHR& a, const VkImageResolve2KHR& b) {
	Matcher m;
	add(m, a.srcSubresource, b.srcSubresource);
	add(m, a.dstSubresource, b.dstSubresource);
	addMemcmp(m, a.srcOffset, b.srcOffset);
	addMemcmp(m, a.dstOffset, b.dstOffset);
	addMemcmp(m, a.extent, b.extent);
	return m;
}

Matcher match(const VkBufferImageCopy2KHR& a, const VkBufferImageCopy2KHR& b) {
	Matcher m;
	add(m, a.bufferImageHeight, b.bufferImageHeight);
	add(m, a.bufferOffset, b.bufferOffset);
	add(m, a.bufferRowLength, b.bufferRowLength);
	addMemcmp(m, a.imageOffset, b.imageOffset);
	addMemcmp(m, a.imageExtent, b.imageExtent);
	add(m, a.imageSubresource, b.imageSubresource);
	return m;
}

Matcher match(const VkImageSubresourceRange& a, const VkImageSubresourceRange& b) {
	Matcher m;
	add(m, a, b);
	return m;
}

Matcher match(const VkClearAttachment& a, const VkClearAttachment& b) {
	if(a.aspectMask != b.aspectMask) {
		return Matcher::noMatch();
	}

	auto sameCV = false;
	if(a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
		if(a.colorAttachment != b.colorAttachment) {
			return Matcher::noMatch();
		}
		// NOTE: stricly speaking we would have to check the format here
		// and compare based on that
		sameCV = std::memcmp(&a.clearValue.color.uint32, &b.clearValue.color.uint32,
			sizeof(a.clearValue.color.uint32)) == 0;
	} else {
		sameCV = std::memcmp(&a.clearValue.depthStencil, &b.clearValue.depthStencil,
			sizeof(a.clearValue.depthStencil)) == 0;
	}

	return sameCV ? Matcher{3.f, 3.f} : Matcher::noMatch();
}

Matcher match(const VkClearRect& a, const VkClearRect& b) {
	Matcher m;
	add(m, a.rect.offset.x, b.rect.offset.x);
	add(m, a.rect.offset.y, b.rect.offset.y);
	add(m, a.rect.extent.width, b.rect.extent.width);
	add(m, a.rect.extent.height, b.rect.extent.height);
	add(m, a.baseArrayLayer, b.baseArrayLayer);
	add(m, a.layerCount, b.layerCount);
	return m;
}

Matcher match(const BoundVertexBuffer& a, const BoundVertexBuffer& b) {
	Matcher m;
	addNonNull(m, a.buffer, b.buffer);
	add(m, a.offset, b.offset, 0.1);
	return m;
}

template<typename ResourceBarrier>
bool equalOwnershipTransition(const ResourceBarrier& a, const ResourceBarrier& b) {
	bool queueTransferA =
		a.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		a.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		a.srcQueueFamilyIndex != a.dstQueueFamilyIndex;
	bool queueTransferB =
		b.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		b.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED &&
		b.srcQueueFamilyIndex != b.dstQueueFamilyIndex;

	if(queueTransferA || queueTransferB) {
		return queueTransferA == queueTransferB &&
				a.srcQueueFamilyIndex == b.srcQueueFamilyIndex &&
				a.dstQueueFamilyIndex == b.dstQueueFamilyIndex;
	}

	return true;
}

// TODO: use fuzzy matching for barriers? I.e. return matcher?
template<typename ImgBarrier>
bool equalImgBarrier(const ImgBarrier& a, const ImgBarrier& b) {
	if(!equalOwnershipTransition(a, b)) {
		return false;
	}

	if constexpr(validExpression<SrcStageMaskMember, ImgBarrier>) {
		if(a.srcStageMask != b.srcStageMask ||
				a.dstStageMask != b.dstStageMask) {
			return false;
		}
	}

	return a.dstAccessMask == b.dstAccessMask &&
		a.srcAccessMask == b.srcAccessMask &&
		a.oldLayout == b.oldLayout &&
		a.newLayout == b.newLayout &&
		a.image == b.image &&
		a.subresourceRange.aspectMask == b.subresourceRange.aspectMask &&
		a.subresourceRange.baseArrayLayer == b.subresourceRange.baseArrayLayer &&
		a.subresourceRange.baseMipLevel == b.subresourceRange.baseMipLevel &&
		a.subresourceRange.layerCount == b.subresourceRange.layerCount &&
		a.subresourceRange.levelCount == b.subresourceRange.levelCount;
}

template<typename BufferBarrier>
bool equalBufBarrier(const BufferBarrier& a, const BufferBarrier& b) {
	if(!equalOwnershipTransition(a, b)) {
		return false;
	}

	if constexpr(validExpression<SrcStageMaskMember, BufferBarrier>) {
		if(a.srcStageMask != b.srcStageMask ||
				a.dstStageMask != b.dstStageMask) {
			return false;
		}
	}

	return a.dstAccessMask == b.dstAccessMask &&
		a.srcAccessMask == b.srcAccessMask &&
		a.buffer == b.buffer &&
		a.size == b.size;
}

bool operator==(const VkMemoryBarrier& a, const VkMemoryBarrier& b) {
	return a.dstAccessMask == b.dstAccessMask && a.srcAccessMask == b.srcAccessMask;
}

bool operator==(const VkMemoryBarrier2& a, const VkMemoryBarrier2& b) {
	return a.dstAccessMask == b.dstAccessMask &&
		a.srcAccessMask == b.srcAccessMask &&
		a.srcStageMask == b.srcStageMask &&
		a.dstStageMask == b.dstStageMask;
}

bool operator==(const VkImageMemoryBarrier& a, const VkImageMemoryBarrier& b) {
	return equalImgBarrier(a, b);
}

bool operator==(const VkImageMemoryBarrier2& a, const VkImageMemoryBarrier2& b) {
	return equalImgBarrier(a, b);
}

bool operator==(const VkBufferMemoryBarrier& a, const VkBufferMemoryBarrier& b) {
	return equalBufBarrier(a, b);
}

bool operator==(const VkBufferMemoryBarrier2& a, const VkBufferMemoryBarrier2& b) {
	return equalBufBarrier(a, b);
}

template<typename T>
void addSpanUnordered(Matcher& m, span<T> a, span<T> b, float weight = 1.0) {
	if(a.empty() && b.empty()) {
		m.match += weight;
		m.total += weight;
		return;
	}

	// TODO: inefficient, find better way of doing this.

	auto count = 0u;
	for(auto i = 0u; i < a.size(); ++i) {
		// check how many times we've seen it already
		auto numSeen = 0u;
		for(auto j = 0u; j < i; ++j) {
			if(a[j] == a[i]) {
				++numSeen;
			}
		}

		// find it in b
		for(auto j = 0u; j < b.size(); ++j) {
			auto found = a[i] == b[j];
			if(!found) {
				continue;
			}

			if(numSeen-- == 0u) {
				++count;
				break;
			}
		}
	}

	m.match += (weight * count) / std::max(a.size(), b.size());
	m.total += weight;

	dlg_assert(m.match <= m.total);
}

template<typename T>
void addSpanOrderedStrict(Matcher& m, span<T> a, span<T> b, float weight = 1.0) {
	m.total += weight;

	if(a.size() != b.size()) {
		return;
	}

	if(a.empty()) {
		return;
	}

	Matcher accum {};
	for(auto i = 0u; i < a.size(); ++i) {
		auto res = match(a[i], b[i]);
		accum.match += res.match;
		accum.total += res.total;
	}

	// TODO: maybe better to make weight also dependent on size?
	// could add flag/param for that behavior.
	m.match += weight * eval(accum);
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

Matcher BarrierCmdBase::doMatch(const BarrierCmdBase& cmd) const {
	Matcher m;

	// NOTE: we match hard on srcStageMask and dstStageMask now, otherwise
	// we get way too many match candidates. With this formulation, we also
	// need at least one common resource
	// add(m, this->srcStageMask, cmd.srcStageMask);
	// add(m, this->dstStageMask, cmd.dstStageMask);
	if(this->srcStageMask != cmd.srcStageMask ||
			this->dstStageMask != cmd.dstStageMask) {
		return Matcher::noMatch();
	}

	addSpanUnordered(m, this->memBarriers, cmd.memBarriers);
	addSpanUnordered(m, this->bufBarriers, cmd.bufBarriers);
	addSpanUnordered(m, this->imgBarriers, cmd.imgBarriers);

	dlg_assert(m.match <= m.total);
	return m;
}

BarrierCmdBase::PatchedBarriers BarrierCmdBase::patchedBarriers(
		ThreadMemScope& memScope, u32 qfam) const {
	PatchedBarriers ret;
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

Matcher Barrier2CmdBase::doMatch(const Barrier2CmdBase& cmd) const {
	Matcher m;

	addSpanUnordered(m, this->memBarriers, cmd.memBarriers);
	addSpanUnordered(m, this->bufBarriers, cmd.bufBarriers);
	addSpanUnordered(m, this->imgBarriers, cmd.imgBarriers);

	if(m.match == 0.f) {
		return Matcher::noMatch();
	}

	add(m, this->flags, cmd.flags);

	dlg_assert(m.match <= m.total);
	return m;
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

Matcher WaitEventsCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const WaitEventsCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	addSpanUnordered(m, events, cmd->events);

	return m;
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

Matcher WaitEvents2Cmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const WaitEvents2Cmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	addSpanUnordered(m, events, cmd->events);

	return m;
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

Matcher BarrierCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const BarrierCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	if(m.match == 0.f || m.total == -1.f) {
		return m;
	}

	add(m, dependencyFlags, cmd->dependencyFlags);
	return m;
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

Matcher Barrier2Cmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const Barrier2Cmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	return doMatch(*cmd);
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

bool same(const RenderPassDesc& a, const RenderPassDesc& b) {
	if(a.subpasses.size() != b.subpasses.size() ||
			a.attachments.size() != b.attachments.size()) {
		return false;
	}

	// compare attachments
	for(auto i = 0u; i < a.attachments.size(); ++i) {
		auto& attA = a.attachments[i];
		auto& attB = b.attachments[i];

		if(attA.format != attB.format ||
				attA.loadOp != attB.loadOp ||
				attA.storeOp != attB.storeOp ||
				attA.initialLayout != attB.initialLayout ||
				attA.finalLayout != attB.finalLayout ||
				attA.stencilLoadOp != attB.stencilLoadOp ||
				attA.stencilStoreOp != attB.stencilStoreOp ||
				attA.samples != attB.samples) {
			return false;
		}
	}

	// compare subpasses
	auto attRefsSame = [](const VkAttachmentReference2& a, const VkAttachmentReference2& b) {
		return a.attachment == b.attachment && (a.attachment == VK_ATTACHMENT_UNUSED ||
				a.aspectMask == b.aspectMask);
	};

	for(auto i = 0u; i < a.subpasses.size(); ++i) {
		auto& subA = a.subpasses[i];
		auto& subB = b.subpasses[i];

		if(subA.colorAttachmentCount != subB.colorAttachmentCount ||
				subA.preserveAttachmentCount != subB.preserveAttachmentCount ||
				bool(subA.pDepthStencilAttachment) != bool(subB.pDepthStencilAttachment) ||
				bool(subA.pResolveAttachments) != bool(subB.pResolveAttachments) ||
				subA.inputAttachmentCount != subB.inputAttachmentCount ||
				subA.pipelineBindPoint != subB.pipelineBindPoint) {
			return false;
		}

		for(auto j = 0u; j < subA.colorAttachmentCount; ++j) {
			if(!attRefsSame(subA.pColorAttachments[j], subB.pColorAttachments[j])) {
				return false;
			}
		}

		for(auto j = 0u; j < subA.inputAttachmentCount; ++j) {
			if(!attRefsSame(subA.pInputAttachments[j], subB.pInputAttachments[j])) {
				return false;
			}
		}

		for(auto j = 0u; j < subA.preserveAttachmentCount; ++j) {
			if(subA.pPreserveAttachments[j] != subB.pPreserveAttachments[j]) {
				return false;
			}
		}

		if(subA.pResolveAttachments) {
			for(auto j = 0u; j < subA.colorAttachmentCount; ++j) {
				if(!attRefsSame(subA.pResolveAttachments[j], subB.pResolveAttachments[j])) {
					return false;
				}
			}
		}

		if(subA.pDepthStencilAttachment &&
				!attRefsSame(*subA.pDepthStencilAttachment, *subB.pDepthStencilAttachment)) {
			return false;
		}
	}

	// TODO: compare dependencies?
	return true;
}

bool addAllowSwapchainViews(Matcher& m, ImageView* va, ImageView* vb) {
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

Matcher BeginRenderPassCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const BeginRenderPassCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;

	// Match render pass description.
	// When a render pass was destroyed, just ignore this check.
	// That is unfortunate, but the attachment check below still
	// provides significant information
	auto rpmatch = false;
	if(rp && cmd->rp) {
		if(!same(rp->desc, cmd->rp->desc)) {
			return Matcher::noMatch();
		}

		// High base match probability since the RenderPasses matched.
		m.total += 4.f;
		m.match += 4.f;
		rpmatch = true;
	}

	// match attachments
	if(attachments.size() != cmd->attachments.size()) {
		return Matcher::noMatch();
	}

	for(auto i = 0u; i < attachments.size(); ++i) {
		// special case: different images but both are of the same
		// swapchain, we treat them as being the same
		auto same = addAllowSwapchainViews(m, attachments[i], cmd->attachments[i]);
		if(!same && !rpmatch) {
			// When we have no renderpass information, we require all attachments
			// to be the same.
			return Matcher::noMatch();
		}
	}

	// TODO: consider render area, clearValues?
	// both might be very dynamic (render area e.g. for dynamic resolution scaling)

	return m;
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

Matcher NextSubpassCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const NextSubpassCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	// we don't need to consider surrounding RenderPass, that is already
	// considered when matching parent
	return cmd->subpassID == subpassID ? Matcher{1.f, 1.f} : Matcher::noMatch();
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
DrawCmdBase::DrawCmdBase(CommandBuffer& cb) :
		state(cb.graphicsState()), pushConstants(cb.pushConstants()) {
}

void DrawCmdBase::displayGrahpicsState(Gui& gui, bool indices) const {
	if(indices) {
		dlg_assert(state.indices.buffer);
		imGuiText("Index Buffer: ");
		ImGui::SameLine();
		refButtonD(gui, state.indices.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}, Type {}", state.indices.offset, vk::name(state.indices.type));
	}

	refButtonD(gui, state.pipe);

	imGuiText("Vertex buffers");
	for(auto& vertBuf : state.vertices) {
		if(!vertBuf.buffer) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, vertBuf.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}", vertBuf.offset);
	}

	// dynamic state
	if(state.pipe && !state.pipe->dynamicState.empty()) {
		imGuiText("DynamicState");
		ImGui::Indent();

		// viewport
		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_VIEWPORT)) {
			auto count = state.pipe->viewportState.viewportCount;
			dlg_assert(state.dynamic.viewports.size() >= count);
			if(count == 1) {
				auto& vp = state.dynamic.viewports[0];
				imGuiText("Viewport: pos ({}, {}), size ({}, {}), depth [{}, {}]",
					vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
			} else if(count > 1) {
				imGuiText("Viewports");
				for(auto& vp : state.dynamic.viewports.first(count)) {
					ImGui::Bullet();
					imGuiText("pos ({}, {}), size ({}, {}), depth [{}, {}]",
						vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
				}
			}
		}
		// scissor
		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_SCISSOR)) {
			auto count = state.pipe->viewportState.scissorCount;
			dlg_assert(state.dynamic.scissors.size() >= count);
			if(count == 1) {
				auto& sc = state.dynamic.scissors[0];
				imGuiText("Scissor: offset ({}, {}), extent ({} {})",
					sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
			} else if(count > 1) {
				imGuiText("Scissors");
				for(auto& sc : state.dynamic.scissors.first(count)) {
					ImGui::Bullet();
					imGuiText("offset ({} {}), extent ({} {})",
						sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height);
				}
			}
		}

		// line width
		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_LINE_WIDTH)) {
			imGuiText("Line width: {}", state.dynamic.lineWidth);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_DEPTH_BIAS)) {
			auto& db = state.dynamic.depthBias;
			imGuiText("Depth bias: constant {}, clamp {}, slope {}",
				db.constant, db.clamp, db.slope);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
			auto& bc = state.dynamic.blendConstants;
			imGuiText("Blend Constants: {} {} {} {}",
				bc[0], bc[1], bc[2], bc[3]);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_DEPTH_BOUNDS)) {
			imGuiText("Depth bounds: [{}, {}]",
				state.dynamic.depthBoundsMin, state.dynamic.depthBoundsMax);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
			imGuiText("Stencil compare mask front: {}{}", std::hex,
				state.dynamic.stencilFront.compareMask);
			imGuiText("Stencil compare mask back: {}{}", std::hex,
				state.dynamic.stencilBack.compareMask);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
			imGuiText("Stencil write mask front: {}{}", std::hex,
				state.dynamic.stencilFront.writeMask);
			imGuiText("Stencil write mask back: {}{}", std::hex,
				state.dynamic.stencilBack.writeMask);
		}

		if(state.pipe->dynamicState.count(VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
			imGuiText("Stencil reference front: {}{}", std::hex,
				state.dynamic.stencilFront.reference);
			imGuiText("Stencil reference back: {}{}", std::hex,
				state.dynamic.stencilBack.reference);
		}

		ImGui::Unindent();
	} else if(!state.pipe) {
		imGuiText("Can't display relevant dynamic state, pipeline was destroyed");
	} else if(state.pipe->dynamicState.empty()) {
		// imGuiText("No relevant dynamic state");
	}
}

Matcher DrawCmdBase::doMatch(const DrawCmdBase& cmd, bool indexed) const {
	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(!state.pipe || !cmd.state.pipe || state.pipe != cmd.state.pipe) {
		return Matcher::noMatch();
	}

	Matcher m;
	m.total += 5.f;
	m.match += 5.f;

	for(auto i = 0u; i < state.pipe->vertexBindings.size(); ++i) {
		dlg_assert(i < state.vertices.size());
		dlg_assert(i < cmd.state.vertices.size());

		addNonNull(m, state.vertices[i].buffer, cmd.state.vertices[i].buffer);

		// Low weight on offset here, it can change frequently for dynamic
		// draw data. But the same buffer is a good indicator for similar
		// commands
		add(m, state.vertices[i].offset, cmd.state.vertices[i].offset, 0.1);
	}

	if(indexed) {
		addNonNull(m, state.indices.buffer, cmd.state.indices.buffer);
		add(m, state.indices.offset, cmd.state.indices.offset, 0.1);

		// different index types is an indicator for fundamentally different
		// commands.
		if(state.indices.type != cmd.state.indices.type) {
			return Matcher::noMatch();
		}
	}

	for(auto& pcr : state.pipe->layout->pushConstants) {
		// TODO: these asserts can trigger if parts of the push constant
		// range was left undefined. It might not be used by the shader
		// anyways. No idea how to fix.
		dlg_assertl_or(dlg_level_warn, pcr.offset + pcr.size <= pushConstants.data.size(), continue);
		dlg_assertl_or(dlg_level_warn, pcr.offset + pcr.size <= cmd.pushConstants.data.size(), continue);

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

Matcher DrawCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DrawCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	// Hard matching on {indexCount, firstIndex, vertexOffset} since that's
	// dependent on the rendered mesh.
	// Soft matching on instancing parameters since renderers might batch dynamically,
	// making this variable at runtime (but still obviously the same command).
	if(vertexCount != cmd->vertexCount || firstVertex != cmd->firstVertex) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd, false);
	if(m.total == -1.f) {
		return m;
	}

	m.total += 5.f;
	m.match += 5.f;
	add(m, instanceCount, cmd->instanceCount, 3.f);
	add(m, firstInstance, cmd->firstInstance, 3.f);

	return m;
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

Matcher DrawIndirectCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DrawIndirectCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	// hard matching on those; differences would indicate a totally
	// different command structure.
	if(cmd->indexed != this->indexed || cmd->stride != this->stride) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd, indexed);
	if(m.total == -1.f) {
		return m;
	}

	m.total += 5.f;
	m.match += 5.f;

	addNonNull(m, buffer, cmd->buffer);

	// we don't hard-match on drawCount since architectures that choose
	// this dynamically per-frame (e.g. for culling) are common
	add(m, drawCount, cmd->drawCount);
	add(m, offset, cmd->offset, 0.2);

	return m;
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

Matcher DrawIndexedCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DrawIndexedCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	// Hard matching on {indexCount, firstIndex, vertexOffset} since that's
	// dependent on the rendered mesh.
	// Soft matching on instancing parameters since renderers might batch dynamically,
	// making this variable at runtime (but still obviously the same command).
	if(cmd->indexCount != indexCount ||
			cmd->firstIndex != firstIndex ||
			cmd->vertexOffset != vertexOffset) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd, true);
	if(m.total == -1.f) {
		return m;
	}

	m.total += 5.f;
	m.match += 5.f;

	add(m, cmd->instanceCount, instanceCount, 3.f);
	add(m, cmd->firstInstance, firstInstance, 3.f);

	return m;
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

Matcher DrawIndirectCountCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DrawIndirectCountCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	// hard matching on those; differences would indicate a totally
	// different command structure.
	if(cmd->indexed != this->indexed || cmd->stride != this->stride) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd, indexed);
	if(m.total == -1.f) {
		return Matcher::noMatch();
	}

	m.match += 2.0;
	m.total += 2.0;

	addNonNull(m, buffer, cmd->buffer);
	addNonNull(m, countBuffer, cmd->countBuffer);

	// we don't hard-match on maxDrawCount since architectures that choose
	// this dynamically per-frame (e.g. for culling) are common
	add(m, maxDrawCount, cmd->maxDrawCount);
	add(m, countBufferOffset, cmd->countBufferOffset, 0.2);
	add(m, offset, cmd->offset, 0.2);

	return m;
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

Matcher BindVertexBuffersCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const BindVertexBuffersCmd*>(&rhs);
	if(!cmd || firstBinding != cmd->firstBinding) {
		return Matcher::noMatch();
	}

	Matcher m;
	m.match += 2.0;
	m.total += 2.0;

	addSpanOrderedStrict(m, buffers, cmd->buffers);

	return m;
}

// BindIndexBufferCmd
void BindIndexBufferCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdBindIndexBuffer(cb, buffer->handle, offset, indexType);
}

Matcher BindIndexBufferCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const BindIndexBufferCmd*>(&rhs);
	if(!cmd || indexType != cmd->indexType) {
		return Matcher::noMatch();
	}

	Matcher m;
	m.match += 1.0;
	m.total += 1.0;
	addNonNull(m, buffer, cmd->buffer);
	add(m, offset, cmd->offset, 0.2);

	return m;
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

Matcher BindDescriptorSetCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const BindDescriptorSetCmd*>(&rhs);
	if(!cmd || firstSet != cmd->firstSet ||
			pipeBindPoint != cmd->pipeBindPoint || pipeLayout != cmd->pipeLayout) {
		return Matcher::noMatch();
	}

	// NOTE: evaluating the used descriptor sets or dynamic offsets
	// is likely of no use as they are too instable.

	Matcher m;
	m.total += 3.f;
	m.match += 3.f;

	return m;
}

// DispatchCmdBase
DispatchCmdBase::DispatchCmdBase(CommandBuffer& cb) :
		state(cb.computeState()), pushConstants(cb.pushConstants()) {
}

void DispatchCmdBase::displayComputeState(Gui& gui) const {
	refButtonD(gui, state.pipe);
}

Matcher DispatchCmdBase::doMatch(const DispatchCmdBase& cmd) const {
	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(!state.pipe || !cmd.state.pipe || state.pipe != cmd.state.pipe) {
		return Matcher::noMatch();
	}

	Matcher m;
	for(auto& pcr : state.pipe->layout->pushConstants) {
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

Matcher DispatchCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DispatchCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	if(m.total == -1.f) {
		return m;
	}

	// we don't hard-match on them since this may change for per-frame
	// varying workloads (in comparison to draw parameters, which rarely
	// change for per-frame stuff). The higher the dimension, the more unlikely
	// this gets though.
	add(m, groupsX, cmd->groupsX, 2.0);
	add(m, groupsY, cmd->groupsY, 4.0);
	add(m, groupsZ, cmd->groupsZ, 6.0);

	return m;
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

Matcher DispatchIndirectCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DispatchIndirectCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	if(m.total == -1.f) {
		return m;
	}

	addNonNull(m, buffer, cmd->buffer);
	add(m, offset, cmd->offset, 0.1);

	return m;
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

Matcher DispatchBaseCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const DispatchBaseCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	if(m.total == -1.f) {
		return m;
	}

	// we don't hard-match on them since this may change for per-frame
	// varying workloads (in comparison to draw parameters, which rarely
	// change for per-frame stuff). The higher the dimension, the more unlikely
	// this gets though.
	add(m, groupsX, cmd->groupsX, 2.0);
	add(m, groupsY, cmd->groupsY, 4.0);
	add(m, groupsZ, cmd->groupsZ, 8.0);

	add(m, baseGroupX, cmd->baseGroupX, 2.0);
	add(m, baseGroupY, cmd->baseGroupY, 4.0);
	add(m, baseGroupZ, cmd->baseGroupZ, 8.0);

	return m;
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

Matcher CopyImageCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const CopyImageCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, dst, cmd->dst, 5);
	addNonNull(m, dst, cmd->dst, 5);
	add(m, srcLayout, cmd->srcLayout);
	add(m, dstLayout, cmd->dstLayout);

	addSpanOrderedStrict(m, copies, cmd->copies);

	return m;
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

Matcher CopyBufferToImageCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const CopyBufferToImageCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, src, cmd->src);
	addNonNull(m, dst, cmd->dst);
	add(m, dstLayout, cmd->dstLayout);
	addSpanOrderedStrict(m, copies, cmd->copies);

	return m;
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

Matcher CopyImageToBufferCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const CopyImageToBufferCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, src, cmd->src);
	addNonNull(m, dst, cmd->dst);
	add(m, srcLayout, cmd->srcLayout);
	addSpanOrderedStrict(m, copies, cmd->copies);

	return m;
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

Matcher BlitImageCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const BlitImageCmd*>(&rhs);
	if(!cmd || filter != cmd->filter) {
		return Matcher::noMatch();
	}

	Matcher m;

	addNonNull(m, src, cmd->src, 5.0);
	addNonNull(m, dst, cmd->dst, 5.0);
	add(m, srcLayout, cmd->srcLayout);
	add(m, dstLayout, cmd->dstLayout);
	addSpanOrderedStrict(m, blits, cmd->blits);

	return m;
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

Matcher ResolveImageCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const ResolveImageCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;

	addNonNull(m, src, cmd->src, 5.0);
	addNonNull(m, dst, cmd->dst, 5.0);
	add(m, srcLayout, cmd->srcLayout);
	add(m, dstLayout, cmd->dstLayout);
	addSpanOrderedStrict(m, regions, cmd->regions);

	return m;
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

Matcher CopyBufferCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const CopyBufferCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, dst, cmd->dst);
	addNonNull(m, dst, cmd->dst);

	addSpanOrderedStrict(m, regions, cmd->regions);

	return m;
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

Matcher UpdateBufferCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const UpdateBufferCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, dst, cmd->dst, 5.0);
	add(m, data.size(), cmd->data.size());
	add(m, offset, cmd->offset, 0.2);
	return m;
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

Matcher FillBufferCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const FillBufferCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, dst, cmd->dst, 5.0);
	add(m, data, cmd->data);
	add(m, size, cmd->size);
	add(m, offset, cmd->offset, 0.1);
	return m;
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

Matcher ClearColorImageCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const ClearColorImageCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, dst, cmd->dst, 5.0);
	add(m, dstLayout, cmd->dstLayout, 2.0);
	addSpanOrderedStrict(m, ranges, cmd->ranges);

	m.total += 1;
	if(std::memcmp(&color, &cmd->color, sizeof(color)) == 0u) {
		m.match += 1;
	}

	return m;
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

Matcher ClearDepthStencilImageCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const ClearDepthStencilImageCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addNonNull(m, dst, cmd->dst, 5.0);
	add(m, dstLayout, cmd->dstLayout, 2.0);
	addSpanOrderedStrict(m, ranges, cmd->ranges);

	m.total += 1;
	if(std::memcmp(&value, &cmd->value, sizeof(value)) == 0u) {
		m.match += 1;
	}

	return m;
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

Matcher ClearAttachmentCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const ClearAttachmentCmd*>(&rhs);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;
	addSpanOrderedStrict(m, attachments, cmd->attachments, 5.0);
	addSpanOrderedStrict(m, rects, cmd->rects);
	return m;
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
		auto* echild = dynamic_cast<ExecuteCommandsChildCmd*>(child);
		dlg_assert(echild);
		dlg_assert(echild->record_->cb);
		vkcbs.push_back(echild->record_->cb->handle());

		child = child->next;
	}

	dev.dispatch.CmdExecuteCommands(cb, u32(vkcbs.size()), vkcbs.data());
	*/
}

void ExecuteCommandsCmd::displayInspector(Gui& gui) const {
	auto echild = dynamic_cast<ExecuteCommandsChildCmd*>(children_);
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

		echild = dynamic_cast<ExecuteCommandsChildCmd*>(echild->next);
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

Matcher BeginDebugUtilsLabelCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const BeginDebugUtilsLabelCmd*>(&rhs);
	if(!cmd || std::strcmp(cmd->name, this->name) != 0) {
		return Matcher::noMatch();
	}

	return Matcher{4.f, 4.f};
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

Matcher BindPipelineCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const BindPipelineCmd*>(&rhs);
	if(!cmd || cmd->bindPoint != this->bindPoint || cmd->pipe != this->pipe) {
		return Matcher::noMatch();
	}

	return Matcher{4.f, 4.f};
}

// PushConstantsCmd
void PushConstantsCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdPushConstants(cb, pipeLayout->handle, stages, offset,
		u32(values.size()), values.data());
}

Matcher PushConstantsCmd::match(const Command& rhs) const {
	auto* cmd = dynamic_cast<const PushConstantsCmd*>(&rhs);

	// hard matching on metadata here. The data is irrelevant when
	// the push destination isn't the same.
	if(!cmd || pipeLayout != cmd->pipeLayout || stages != cmd->stages ||
			offset != cmd->offset || values.size() != cmd->values.size()) {
		return Matcher::noMatch();
	}

	Matcher m;
	m.total += 4.f;
	m.match += 4.f;

	if(std::memcmp(values.data(), cmd->values.data(), values.size()) == 0u) {
		m.total += 1.f;
		m.match += 1.f;
	}

	return m;
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
void CopyAccelStructureCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	VkCopyAccelerationStructureInfoKHR info {};
	info.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
	info.pNext = pNext;
	info.dst = src->handle;
	info.src = dst->handle;
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
BuildAccelStructsCmd::BuildAccelStructsCmd(CommandBuffer& cb) {
	(void) cb;
}

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

BuildAccelStructsIndirectCmd::BuildAccelStructsIndirectCmd(CommandBuffer& cb) {
	(void) cb;
}

void BuildAccelStructsIndirectCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdBuildAccelerationStructuresIndirectKHR(cb, u32(buildInfos.size()),
		buildInfos.data(), indirectAddresses.data(), indirectStrides.data(),
		maxPrimitiveCounts.data());
}

// VK_KHR_ray_tracing_pipeline
TraceRaysCmdBase::TraceRaysCmdBase(CommandBuffer& cb) :
		state(cb.rayTracingState()), pushConstants(cb.pushConstants()) {
}

Matcher TraceRaysCmdBase::doMatch(const TraceRaysCmdBase& cmd) const {
	// different pipelines means the draw calls are fundamentally different,
	// no matter if similar data is bound.
	if(!state.pipe || !cmd.state.pipe || state.pipe != cmd.state.pipe) {
		return Matcher::noMatch();
	}

	Matcher m;
	for(auto& pcr : state.pipe->layout->pushConstants) {
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

Matcher TraceRaysCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const TraceRaysCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	if(m.total == -1.f) {
		return m;
	}

	// we don't hard-match on them since this may change for per-frame
	// varying workloads (in comparison to draw parameters, which rarely
	// change for per-frame stuff). The higher the dimension, the more unlikely
	// this gets though.
	add(m, width, cmd->width, 2.0);
	add(m, height, cmd->height, 4.0);
	add(m, depth, cmd->depth, 6.0);

	// TODO: consider bound tables? At least size and stride?

	return m;
}

void TraceRaysIndirectCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdTraceRaysIndirectKHR(cb,
		&raygenBindingTable, &missBindingTable, &hitBindingTable, &callableBindingTable,
		indirectDeviceAddress);
}

Matcher TraceRaysIndirectCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const TraceRaysCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	auto m = doMatch(*cmd);
	if(m.total == -1.f) {
		return m;
	}

	// TODO: consider bound tables? At least size and stride?

	return m;
}

void SetRayTracingPipelineStackSizeCmd::record(const Device& dev, VkCommandBuffer cb, u32) const {
	dev.dispatch.CmdSetRayTracingPipelineStackSizeKHR(cb, stackSize);
}

void BeginRenderingCmd::record(const Device& dev, VkCommandBuffer cb,
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

		if(src.resolveView) {
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
	this->record(dev, cb, std::nullopt, std::nullopt);
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

Matcher BeginRenderingCmd::match(const Command& base) const {
	auto* cmd = dynamic_cast<const BeginRenderingCmd*>(&base);
	if(!cmd) {
		return Matcher::noMatch();
	}

	Matcher m;

	// Match attachments. We are strict here, we don't have any other information
	if(colorAttachments.size() != cmd->colorAttachments.size()) {
		return Matcher::noMatch();
	}

	auto addAttachment = [&](const Attachment& a, const Attachment& b) {
		if(!a.view && !b.view) {
			add(m, a.view, b.view);
			return true;
		}

		if((a.resolveView || b.resolveView) && (
				!addAllowSwapchainViews(m, a.resolveView, b.resolveView) ||
				a.resolveMode != b.resolveMode ||
				a.resolveImageLayout != b.resolveImageLayout)) {
			return false;
		}

		// NOTE: clear value irrelevant, might change from frame to frame
		return addAllowSwapchainViews(m, a.view, b.view) &&
			a.loadOp == b.loadOp &&
			a.storeOp == b.storeOp &&
			a.imageLayout == b.imageLayout;
	};

	for(auto i = 0u; i < colorAttachments.size(); ++i) {
		if(!addAttachment(colorAttachments[i], cmd->colorAttachments[i])) {
			return Matcher::noMatch();
		}
	}

	if(!addAttachment(depthAttachment, cmd->depthAttachment) ||
			!addAttachment(stencilAttachment, cmd->stencilAttachment)) {
		return Matcher::noMatch();
	}

	if(flags != cmd->flags ||
			viewMask != cmd->viewMask ||
			layerCount != cmd->layerCount) {
		return Matcher::noMatch();
	}

	m.match += 4.f;
	m.total += 4.f;
	return m;
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
	auto drawIndirectCmd = dynamic_cast<const DrawIndirectCmd*>(&cmd);
	auto dispatchIndirectCmd = dynamic_cast<const DispatchIndirectCmd*>(&cmd);
	auto drawIndirectCountCmd = dynamic_cast<const DrawIndirectCountCmd*>(&cmd);
	auto traceRaysIndirectCmd = dynamic_cast<const TraceRaysIndirectCmd*>(&cmd);

	return dispatchIndirectCmd ||
		drawIndirectCmd ||
		drawIndirectCountCmd ||
		traceRaysIndirectCmd;
}

} // namespace vil
