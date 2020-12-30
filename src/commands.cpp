#include <commands.hpp>
#include <handles.hpp>
#include <cb.hpp>
#include <span.hpp>
#include <util.hpp>
#include <gui/gui.hpp>
#include <imguiutil.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <vk/enumString.hpp>

namespace fuen {

// Command utility
template<typename C>
auto rawHandles(const C& handles) {
	using VkH = decltype(handle(*handles[0]));
	std::vector<VkH> ret;
	ret.reserve(handles.size());
	for(auto* h : handles) {
		ret.push_back(handle(*h));
	}

	return ret;
}

template<typename H>
void checkUnset(H*& handlePtr, const std::unordered_set<DeviceHandle*>& destroyed) {
	if(handlePtr&& destroyed.count(handlePtr)) {
		handlePtr = nullptr;
	}
}

template<typename H>
void checkUnset(span<H*> handlePtr, const std::unordered_set<DeviceHandle*>& destroyed) {
	for(auto& ptr : handlePtr) {
		checkUnset(ptr, destroyed);
	}
}

NameResult name(DeviceHandle* handle, NullName nullName) {
	if(!handle) {
		switch(nullName) {
			case NullName::null: return {NameType::null, "<null>"};
			case NullName::destroyed: return {NameType::null, "<destroyed>"};
			case NullName::empty: return {NameType::null, ""};

		}
	}

	auto name = fuen::name(*handle);
	if(handle->name.empty()) {
		return {NameType::unnamed, name};
	}

	return {NameType::named, name};
}

std::vector<std::string> createArgumentsDesc() {
	return {};
}

void addToArgumentsDesc(std::vector<std::string>& ret, const char* str) {
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const std::string& str) {
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundVertexBuffer& buf) {
	if(!buf.buffer) {
		return;
	}

	auto str = dlg::format("v {}[{}]", name(buf.buffer).name, buf.offset);
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundIndexBuffer& buf) {
	if(!buf.buffer) {
		return;
	}

	auto str = dlg::format("{}[{}]", name(buf.buffer).name, buf.offset);
	ret.push_back(str);
	ret.push_back(vk::name(buf.type));
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundDescriptorSet& set) {
	if(!set.ds) {
		return;
	}

	ret.push_back(name(set.ds).name);
}

template<typename H> using FuenNameExpr = decltype(std::declval<H>().objectType);
template<typename H> using FuenNameExprPtr = decltype(std::declval<H>()->objectType);
template<typename H> using VkNameExpr = decltype(vk::name(std::declval<H>()));
template<typename H> using ToStringExpr = decltype(std::to_string(std::declval<H>()));

template<typename T, typename = std::enable_if_t<
	validExpression<FuenNameExpr, T> ||
	validExpression<FuenNameExprPtr, T> ||
	validExpression<ToStringExpr, T> ||
	validExpression<VkNameExpr, T>>>
void addToArgumentsDesc(std::vector<std::string>& ret, const T& val) {
	if constexpr(validExpression<FuenNameExpr, T>) {
		ret.push_back(fuen::name(val));
	} else if constexpr(validExpression<FuenNameExprPtr, T>) {
		ret.push_back(val ? fuen::name(*val) : "null");
	} else if constexpr(validExpression<VkNameExpr, T>) {
		ret.push_back(vk::name(val));
	} else if constexpr(validExpression<ToStringExpr, T>) {
		ret.push_back(std::to_string(val));
	} else {
		static_assert(templatize<T>(false), "Invalid type");
	}
}

template<typename T>
void addToArgumentsDesc(std::vector<std::string>& ret, span<T> values) {
	for(auto& val : values) {
		addToArgumentsDesc(ret, val);
	}
}

template<typename T>
void addToArgumentsDesc(std::vector<std::string>& ret, const std::vector<T>& values) {
	for(auto& val : values) {
		addToArgumentsDesc(ret, val);
	}
}

template<typename First, typename... Rest>
std::vector<std::string> createArgumentsDesc(const First& first, const Rest&... rest) {
	auto ret = createArgumentsDesc(rest...);
	addToArgumentsDesc(ret, first);
	return ret;
}

std::vector<const Command*> displayCommands(const Command* cmd,
		const Command* selected, Command::TypeFlags typeFlags) {
	// TODO: should use imgui list clipper, might have *a lot* of commands here.
	// But first we have to restrict what cmd->display can actually do.
	// Would also have to pre-filter command for that.
	std::vector<const Command*> ret;
	while(cmd) {
		if((typeFlags & cmd->type())) {
			ImGui::Separator();
			if(auto reti = cmd->display(selected, typeFlags); !reti.empty()) {
				dlg_assert(ret.empty());
				ret = reti;
			}
		}

		cmd = cmd->next;
	}

	return ret;
}


// Command
std::vector<const Command*> Command::display(const Command* sel, TypeFlags typeFlags) const {
	if(!(typeFlags & this->type())) {
		return {};
	}

	int flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
	if(sel == this) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	auto idStr = dlg::format("{}:{}", nameDesc(), relID);
	ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", toString().c_str());

	std::vector<const Command*> ret;
	if(ImGui::IsItemClicked()) {
		ret = {this};
	}

	ImGui::TreePop();

	return ret;
}

// WaitEventsCmd
void WaitEventsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkEvents = rawHandles(this->events);
	dev.dispatch.CmdWaitEvents(cb,
		u32(vkEvents.size()), vkEvents.data(),
		this->srcStageMask, this->dstStageMask,
		u32(this->memBarriers.size()), this->memBarriers.data(),
		u32(this->bufBarriers.size()), this->bufBarriers.data(),
		u32(this->imgBarriers.size()), this->imgBarriers.data());

}

// Commands
std::vector<const Command*> ParentCommand::display(const Command* selected,
		TypeFlags typeFlags, const Command* cmd) const {
	// auto flags = ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;
	auto flags = 0u;
	if(this == selected) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	std::vector<const Command*> ret {};
	auto idStr = dlg::format("{}:{}", nameDesc(), relID);
	auto open = ImGui::TreeNodeEx(idStr.c_str(), flags, "%s", toString().c_str());
	if(ImGui::IsItemClicked()) {
		ret = {this};
	}

	if(open) {
		auto retc = displayCommands(cmd, selected, typeFlags);
		if(!retc.empty()) {
			dlg_assert(ret.empty());
			ret = std::move(retc);
			ret.push_back(this);
		}

		ImGui::TreePop();
	}

	return ret;
}

std::vector<const Command*> ParentCommand::display(const Command* selected,
		TypeFlags typeFlags) const {
	return this->display(selected, typeFlags, children());
}

std::vector<const Command*> BeginRenderPassCmd::display(const Command* selected,
		TypeFlags typeFlags) const {
	auto cmd = this->children_;
	auto first = static_cast<FirstSubpassCmd*>(nullptr);
	if(cmd) {
		// If we only have one subpass, don't give it an extra section
		// to make everything more compact.
		first = dynamic_cast<FirstSubpassCmd*>(cmd);
		dlg_assert(first);
		if(!first->next) {
			cmd = first->children_;
		}
	}

	auto ret = ParentCommand::display(selected, typeFlags, cmd);
	if(ret.size() > 1 && first) {
		ret.insert(ret.end() - 1, first);
	}

	return ret;
}

void BarrierCmdBase::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffers, destroyed);
	checkUnset(images, destroyed);
}

std::vector<std::string> WaitEventsCmd::argumentsDesc() const {
	return createArgumentsDesc(events,
		vk::flagNames(VkPipelineStageFlagBits(srcStageMask)),
		vk::flagNames(VkPipelineStageFlagBits(dstStageMask)));
}

void WaitEventsCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	BarrierCmdBase::unset(destroyed);
	checkUnset(events, destroyed);
}

void BarrierCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPipelineBarrier(cb,
		this->dependencyFlags,
		this->srcStageMask, this->dstStageMask,
		u32(this->memBarriers.size()), this->memBarriers.data(),
		u32(this->bufBarriers.size()), this->bufBarriers.data(),
		u32(this->imgBarriers.size()), this->imgBarriers.data());

}

std::vector<std::string> BarrierCmd::argumentsDesc() const {
	return createArgumentsDesc(
		vk::flagNames(VkPipelineStageFlagBits(srcStageMask)),
		vk::flagNames(VkPipelineStageFlagBits(dstStageMask)));
}

// BeginRenderPassCmd
std::string BeginRenderPassCmd::toString() const {
	auto [fbRes, fbName] = name(fb);
	auto [rpRes, rpName] = name(rp);
	if(fbRes == NameType::named && rpRes == NameType::named) {
		return dlg::format("BeginRenderPass({}, {})", rpName, fbName);
	} else if(rpRes == NameType::named) {
		return dlg::format("BeginRenderPass({})", rpName);
	} else {
		return "BeginRenderPass";
	}
}

void BeginRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(this->subpassBeginInfo.pNext) {
		auto f = selectCmd(dev.dispatch.CmdBeginRenderPass2, dev.dispatch.CmdBeginRenderPass2KHR);
		f(cb, &this->info, &this->subpassBeginInfo);
	} else {
		dev.dispatch.CmdBeginRenderPass(cb, &this->info, this->subpassBeginInfo.contents);
	}
}

std::vector<std::string> BeginRenderPassCmd::argumentsDesc() const {
	return createArgumentsDesc(rp, subpassBeginInfo.contents);
}

void BeginRenderPassCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(rp, destroyed);
	checkUnset(fb, destroyed);
	ParentCommand::unset(destroyed);
}

void BeginRenderPassCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, fb);
	refButtonD(gui, rp);
}

void NextSubpassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(this->beginInfo.pNext || this->endInfo.pNext) {
		auto f = selectCmd(dev.dispatch.CmdNextSubpass2, dev.dispatch.CmdNextSubpass2KHR);
		f(cb, &this->beginInfo, &this->endInfo);
	} else {
		dev.dispatch.CmdNextSubpass(cb, this->beginInfo.contents);
	}
}

void EndRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(this->endInfo.pNext) {
		auto f = selectCmd(dev.dispatch.CmdEndRenderPass2, dev.dispatch.CmdEndRenderPass2KHR);
		f(cb, &this->endInfo);
	} else {
		dev.dispatch.CmdEndRenderPass(cb);
	}
}

// DrawCmdBase
DrawCmdBase::DrawCmdBase(CommandBuffer& cb, const GraphicsState& gfxState) {
	state = copy(cb, gfxState);
	// NOTE: only do this when pipe layout matches pcr layout
	pushConstants.data = copySpan(cb, cb.pushConstants().data);
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

	imGuiText("Verex buffers");
	for(auto& vertBuf : state.vertices) {
		if(!vertBuf.buffer) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, vertBuf.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}", vertBuf.offset);
	}

	imGuiText("Descriptors");
	for(auto& ds : state.descriptorSets) {
		if(!ds.ds) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, ds.ds, "<Invalid>");
		// TODO: dynamic offsets
	}

	// TODO: push constants
}

void DrawCmdBase::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(state.pipe, destroyed);
	checkUnset(state.indices.buffer, destroyed);

	for(auto& verts : state.vertices) {
		checkUnset(verts.buffer, destroyed);
	}

	for(auto& ds : state.descriptorSets) {
		checkUnset(ds.ds, destroyed);
		// ds.layout is kept alive
	}
}

// DrawCmd
void DrawCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

std::vector<std::string> DrawCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.vertices, state.descriptorSets,
		vertexCount, instanceCount, firstInstance, firstVertex);
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
void DrawIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(indexed) {
		dev.dispatch.CmdDrawIndexedIndirect(cb, buffer->handle, offset, drawCount, stride);
	} else {
		dev.dispatch.CmdDrawIndirect(cb, buffer->handle, offset, drawCount, stride);
	}
}

std::vector<std::string> DrawIndirectCmd::argumentsDesc() const {
	auto ret = createArgumentsDesc(buffer, offset, drawCount, stride,
		state.pipe, state.vertices, state.descriptorSets);
	if(indexed) {
		addToArgumentsDesc(ret, state.indices);
	}

	return ret;
}

void DrawIndirectCmd::displayInspector(Gui& gui) const {
	// TODO: display effective draw command
	refButtonD(gui, buffer);
	DrawCmdBase::displayGrahpicsState(gui, false);
}

void DrawIndirectCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
	DrawCmdBase::unset(destroyed);
}

std::string DrawIndirectCmd::toString() const {
	auto [bufNameRes, bufName] = name(buffer);
	auto cmdName = indexed ? "DrawIndexedIndirect" : "DrawIndirect";
	if(bufNameRes == NameType::named) {
		return dlg::format("{}({}, {})", cmdName, bufName, drawCount);
	} else {
		return dlg::format("{}({})", cmdName, drawCount);
	}
}

// DrawIndexedCmd
void DrawIndexedCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

std::vector<std::string> DrawIndexedCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.indices,
		state.vertices, state.descriptorSets,
		indexCount, instanceCount, firstInstance, firstIndex);
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
void DrawIndirectCountCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(indexed) {
		auto f = selectCmd(
			dev.dispatch.CmdDrawIndexedIndirectCount,
			dev.dispatch.CmdDrawIndexedIndirectCountKHR,
			dev.dispatch.CmdDrawIndexedIndirectCountAMD);
		f(cb, buffer->handle, offset, countBuffer->handle, countBufferOffset,
			maxDrawCount, stride);
	} else {
		auto f = selectCmd(
			dev.dispatch.CmdDrawIndirectCount,
			dev.dispatch.CmdDrawIndirectCountKHR,
			dev.dispatch.CmdDrawIndirectCountAMD);
		f(cb, buffer->handle, offset,
			countBuffer->handle, countBufferOffset, maxDrawCount, stride);
	}
}

std::vector<std::string> DrawIndirectCountCmd::argumentsDesc() const {
	auto ret = createArgumentsDesc(buffer, offset, countBuffer, countBufferOffset,
		maxDrawCount, stride, state.pipe, state.vertices, state.descriptorSets);
	if(indexed) {
		addToArgumentsDesc(ret, state.indices);
	}

	return ret;
}

void DrawIndirectCountCmd::displayInspector(Gui& gui) const {
	// TODO: display effective draw command
	imGuiText("Indirect buffer:");
	ImGui::SameLine();
	refButtonD(gui, buffer);

	imGuiText("Count buffer:");
	ImGui::SameLine();
	refButtonD(gui, countBuffer);

	DrawCmdBase::displayGrahpicsState(gui, false);
}

void DrawIndirectCountCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
	checkUnset(countBuffer, destroyed);
	DrawCmdBase::unset(destroyed);
}

// BindVertexBuffersCmd
void BindVertexBuffersCmd::record(const Device& dev, VkCommandBuffer cb) const {
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
		auto [buf0NameRes, buf0Name] = name(buffers[0].buffer);
		if(buf0NameRes == NameType::named) {
			return dlg::format("BindVertexBuffers({}: {})", firstBinding, buf0Name);
		} else {
			return dlg::format("BindVertexBuffers({})", firstBinding);
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

void BindVertexBuffersCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	for(auto& buf : buffers) {
		checkUnset(buf.buffer, destroyed);
	}
}

// BindIndexBufferCmd
void BindIndexBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindIndexBuffer(cb, buffer->handle, offset, indexType);
}

void BindIndexBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
}

// BindDescriptorSetCmd
void BindDescriptorSetCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkds = rawHandles(sets);
	dev.dispatch.CmdBindDescriptorSets(cb, pipeBindPoint, pipeLayout->handle,
		firstSet, u32(vkds.size()), vkds.data(),
		u32(dynamicOffsets.size()), dynamicOffsets.data());
}

std::string BindDescriptorSetCmd::toString() const {
	if(sets.size() == 1) {
		auto [ds0Res, ds0Name] = name(sets[0]);
		if(ds0Res == NameType::named) {
			return dlg::format("BindDescriptorSets({}: {})", firstSet, ds0Name);
		} else {
			return dlg::format("BindDescriptorSets({})", firstSet);
		}
	} else {
		return dlg::format("BindDescriptorSets({}..{})",
			firstSet, firstSet + sets.size() - 1);
	}
}

void BindDescriptorSetCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(sets, destroyed);
}

// DispatchCmdBase
DispatchCmdBase::DispatchCmdBase(CommandBuffer& cb, const ComputeState& compState) {
	state = copy(cb, compState);
	// NOTE: only do this when pipe layout matches pcr layout
	pushConstants.data = copySpan(cb, cb.pushConstants().data);
}

void DispatchCmdBase::displayComputeState(Gui& gui) const {
	refButtonD(gui, state.pipe);

	imGuiText("Descriptors");
	for(auto& ds : state.descriptorSets) {
		if(!ds.ds) {
			imGuiText("null");
			continue;
		}

		refButtonD(gui, ds.ds, "<Invalid>");
		// TODO: dynamic offsets
	}

	// TODO: push constants
}

void DispatchCmdBase::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(state.pipe, destroyed);
	for(auto& ds : state.descriptorSets) {
		checkUnset(ds.ds, destroyed);
		// ds.layout kept alive
	}
}

// DispatchCmd
void DispatchCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatch(cb, groupsX, groupsY, groupsZ);
}

std::vector<std::string> DispatchCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.descriptorSets, groupsX, groupsY, groupsZ);
}

std::string DispatchCmd::toString() const {
	return dlg::format("Dispatch({}, {}, {})", groupsX, groupsY, groupsZ);
}

void DispatchCmd::displayInspector(Gui& gui) const {
	imGuiText("Groups: {} {} {}", groupsX, groupsY, groupsZ);
	DispatchCmdBase::displayComputeState(gui);
}

// DispatchIndirectCmd
void DispatchIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatchIndirect(cb, buffer->handle, offset);
}

void DispatchIndirectCmd::displayInspector(Gui& gui) const {
	// TODO: display effective dispatch command
	refButtonD(gui, buffer);
	DispatchCmdBase::displayComputeState(gui);
}

std::vector<std::string> DispatchIndirectCmd::argumentsDesc() const {
	return createArgumentsDesc(buffer, offset, state.pipe, state.descriptorSets);
}

void DispatchIndirectCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(buffer, destroyed);
	DispatchCmdBase::unset(destroyed);
}

// DispatchBaseCmd
void DispatchBaseCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto f = selectCmd(dev.dispatch.CmdDispatchBase, dev.dispatch.CmdDispatchBaseKHR);
	f(cb, baseGroupX, baseGroupY, baseGroupZ, groupsX, groupsY, groupsZ);
}

std::vector<std::string> DispatchBaseCmd::argumentsDesc() const {
	return createArgumentsDesc(baseGroupX, baseGroupY, baseGroupZ,
		groupsX, groupsY, groupsZ);
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
void CopyImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		u32(copies.size()), copies.data());
}

std::string CopyImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyImage({} -> {})", srcName, dstName);
	} else {
		return "CopyImage";
	}
}

void CopyImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

void CopyImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	// TODO: copies
}

// CopyBufferToImageCmd
void CopyBufferToImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBufferToImage(cb, src->handle, dst->handle,
		imgLayout, u32(copies.size()), copies.data());
}

void CopyBufferToImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	// TODO: copies
}

std::string CopyBufferToImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyBufferToImage({} -> {})", srcName, dstName);
	} else {
		return "CopyBufferToImage";
	}
}

void CopyBufferToImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

// CopyImageToBufferCmd
void CopyImageToBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImageToBuffer(cb, src->handle, imgLayout, dst->handle,
		u32(copies.size()), copies.data());
}

void CopyImageToBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	// TODO: copies
}

std::string CopyImageToBufferCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyImageToBuffer({} -> {})", srcName, dstName);
	} else {
		return "CopyImageToBuffer";
	}
}

void CopyImageToBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

// BlitImageCmd
void BlitImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBlitImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		u32(blits.size()), blits.data(), filter);
}

void BlitImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	// TODO: filter
	// TODO: blits
}

std::string BlitImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("BlitImage({} -> {})", srcName, dstName);
	} else {
		return "BlitImage";
	}
}

void BlitImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

// ResolveImageCmd
void ResolveImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResolveImage(cb, src->handle, srcLayout,
		dst->handle, dstLayout, u32(regions.size()), regions.data());
}

void ResolveImageCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	// TODO: regions & layouts
}

std::string ResolveImageCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("ResolveImage({} -> {})", srcName, dstName);
	} else {
		return "ResolveImage";
	}
}

void ResolveImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

// CopyBufferCmd
void CopyBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBuffer(cb, src->handle, dst->handle,
		u32(regions.size()), regions.data());
}

void CopyBufferCmd::displayInspector(Gui& gui) const {
	refButtonD(gui, src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	refButtonD(gui, dst);

	// TODO: copies
}

std::string CopyBufferCmd::toString() const {
	auto [srcRes, srcName] = name(src);
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named || srcRes == NameType::named) {
		return dlg::format("CopyBuffer({} -> {})", srcName, dstName);
	} else {
		return "CopyBuffer";
	}
}

void CopyBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(src, destroyed);
	checkUnset(dst, destroyed);
}

// UpdateBufferCmd
void UpdateBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdUpdateBuffer(cb, dst->handle, offset, data.size(), data.data());
}

void UpdateBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

std::string UpdateBufferCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("UpdateBuffer({})", dstName);
	} else {
		return "UpdateBuffer";
	}
}

// FillBufferCmd
void FillBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdFillBuffer(cb, dst->handle, offset, size, data);
}

void FillBufferCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

std::string FillBufferCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("FillBuffer({})", dstName);
	} else {
		return "FillBuffer";
	}
}

// ClearColorImageCmd
void ClearColorImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearColorImage(cb, dst->handle, imgLayout, &color,
		u32(ranges.size()), ranges.data());
}

std::string ClearColorImageCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("ClearColorImage({})", dstName);
	} else {
		return "ClearColorImage";
	}
}

void ClearColorImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

// ClearDepthStencilImageCmd
void ClearDepthStencilImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearDepthStencilImage(cb, dst->handle, imgLayout, &value,
		u32(ranges.size()), ranges.data());
}

std::string ClearDepthStencilImageCmd::toString() const {
	auto [dstRes, dstName] = name(dst);
	if(dstRes == NameType::named) {
		return dlg::format("ClearDepthStencilImage({})", dstName);
	} else {
		return "ClearColorImage";
	}
}

void ClearDepthStencilImageCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(dst, destroyed);
}

// Clear AttachhmentCmd
void ClearAttachmentCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearAttachments(cb, u32(attachments.size()),
		attachments.data(), u32(rects.size()), rects.data());
}

// SetEventCmd
void SetEventCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetEvent(cb, event->handle, stageMask);
}

void SetEventCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(event, destroyed);
}

// ResetEventCmd
void ResetEventCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResetEvent(cb, event->handle, stageMask);
}

void ResetEventCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(event, destroyed);
}

void ExecuteCommandsCmd::record(const Device& dev, VkCommandBuffer cb) const {
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
}

std::vector<const Command*> ExecuteCommandsCmd::display(const Command* selected,
		TypeFlags typeFlags) const {
	auto cmd = this->children_;
	auto first = static_cast<ExecuteCommandsChildCmd*>(nullptr);
	if(cmd) {
		// If we only have one subpass, don't give it an extra section
		// to make everything more compact.
		first = dynamic_cast<ExecuteCommandsChildCmd*>(cmd);
		dlg_assert(first);
		if(!first->next) {
			cmd = first->record_->commands;
		}
	}

	auto ret = ParentCommand::display(selected, typeFlags, cmd);
	if(ret.size() > 1 && first) {
		ret.insert(ret.end() - 1, first);
	}

	return ret;
}

std::string ExecuteCommandsChildCmd::toString() const {
	auto [cbRes, cbName] = name(record_->cb);
	if(cbRes == NameType::named) {
		return dlg::format("{}: {}", id_, cbName);
	} else {
		return dlg::format("{}", id_);
	}
}

void ExecuteCommandsCmd::displayInspector(Gui& gui) const {
	auto echild = dynamic_cast<ExecuteCommandsChildCmd*>(children_);
	while(echild) {
		// TODO: could link to command buffer (if still valid/linked)
		auto label = dlg::format("View Recording {}", echild->id_);
		if(ImGui::Button(label.c_str())) {
			gui.selectCommands(IntrusivePtr<CommandRecord>(echild->record_), false);
		}

		echild = dynamic_cast<ExecuteCommandsChildCmd*>(echild->next);
	}
}

// BeginDebugUtilsLabelCmd
void BeginDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	VkDebugUtilsLabelEXT label {};
	label.pLabelName = this->name;
	std::memcpy(&label.color, this->color.data(), sizeof(label.color));
	dev.dispatch.CmdBeginDebugUtilsLabelEXT(cb, &label);
}

// EndDebugUtilsLabelCmd
void EndDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndDebugUtilsLabelEXT(cb);
}

// BindPipelineCmd
void BindPipelineCmd::record(const Device& dev, VkCommandBuffer cb) const {
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
	auto [nameRes, pipeName] = name(pipe);
	if(nameRes == NameType::named) {
		return dlg::format("BindPipeline({}, {})", bp, pipeName);
	} else {
		return dlg::format("BindPipeline({})", bp);
	}
}

void BindPipelineCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pipe, destroyed);
}

void PushConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPushConstants(cb, pipeLayout->handle, stages, offset,
		u32(values.size()), values.data());
}

void SetViewportCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetViewport(cb, first, u32(viewports.size()), viewports.data());
}

void SetScissorCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetScissor(cb, first, u32(scissors.size()), scissors.data());
}

void SetLineWidthCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetLineWidth(cb, width);
}

void SetDepthBiasCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetDepthBias(cb, state.constant, state.clamp, state.slope);
}

void SetBlendConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetBlendConstants(cb, values.data());
}

void SetStencilCompareMaskCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilCompareMask(cb, faceMask, value);
}

void SetStencilWriteMaskCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilWriteMask(cb, faceMask, value);
}

void SetStencilReferenceCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetStencilReference(cb, faceMask, value);
}

void SetDepthBoundsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetDepthBounds(cb, min, max);
}

// BeginQuery
void BeginQueryCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBeginQuery(cb, pool->handle, query, flags);
}

void BeginQueryCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// EndQuery
void EndQueryCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndQuery(cb, pool->handle, query);
}

void EndQueryCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// ResetQuery
void ResetQueryPoolCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResetQueryPool(cb, pool->handle, first, count);
}

void ResetQueryPoolCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// WriteTimestamp
void WriteTimestampCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdWriteTimestamp(cb, stage, pool->handle, query);
}

void WriteTimestampCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
}

// CopyQueryPool
void CopyQueryPoolResultsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyQueryPoolResults(cb, pool->handle, first, count,
		dstBuffer->handle, dstOffset, stride, flags);
}

void CopyQueryPoolResultsCmd::unset(const std::unordered_set<DeviceHandle*>& destroyed) {
	checkUnset(pool, destroyed);
	checkUnset(dstBuffer, destroyed);
}

} // namespace fuen
