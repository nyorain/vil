#include <commands.hpp>
#include <cb.hpp>
#include <span.hpp>
#include <util.hpp>
#include <gui/gui.hpp>

namespace fuen {

// CommandDescription
void fillIn(CommandDescription& ret, const CommandBuffer& cb, const Command& cmd) {
	ret.command = cmd.nameDesc();
	ret.arguments = cmd.argumentsDesc();

	auto* siblings = &cb.commands;
	if(cmd.parent) {
		siblings = &cmd.parent->children;
	}

	bool before = true;
	for(auto& sibling : *siblings) {
		if(sibling.get() == &cmd) {
			before = false;
		}

		if(sibling->nameDesc() == ret.command) {
			++ret.count;

			if(before) {
				++ret.id;
			}
		}
	}

	dlg_assert(!before);
}

std::vector<CommandDescription> CommandDescription::get(const CommandBuffer& cb, const Command& cmd) {
	dlg_assert(cb.state == CommandBuffer::State::executable);

	std::vector<CommandDescription> ret;
	fillIn(ret.emplace_back(), cb, cmd);

	// find parents
	auto* parent = cmd.parent;
	while(parent) {
		fillIn(ret.emplace_back(), cb, *parent);
		parent = parent->parent;
	}

	std::reverse(ret.begin(), ret.end());
	return ret;
}

Command* CommandDescription::find(const CommandBuffer& cb, span<const CommandDescription> desc) {
	dlg_assert(cb.state == CommandBuffer::State::executable);

	if(desc.empty()) {
		return nullptr;
	}

	struct Candidate {
		Command* command {};
		float score {};

		bool operator<(const Candidate& other) const {
			return score < other.score;
		}
	};

	// TODO: when we can't find an exact match, we probably want to return
	// the nearest parent we could find (maybe require an even higher
	// threshold though since jumping to a false parent sucks).
	// Maybe control that behavior via an external argument
	auto findCandidates = [](const std::vector<std::unique_ptr<Command>>& cmds,
			const CommandDescription& desc) -> std::vector<Candidate> {
		std::vector<Candidate> candidates;
		for(auto& cmd : cmds) {
			if(cmd->nameDesc() != desc.command) {
				continue;
			}

			candidates.push_back({cmd.get(), 0.f});
		}

		for(auto c = 0u; c < candidates.size(); ++c) {
			auto& cand = candidates[c];

			// TODO: we can do better!
			// This is very sensitive to just erasing large chunks of data.
			// We can use desc.count to get a more precise score.
			auto args = cand.command->argumentsDesc();
			u32 numSame = 0u;
			for(auto i = 0u; i < std::min(args.size(), desc.arguments.size()); ++i) {
				if(args[i] == desc.arguments[i]) {
					++numSame;
				}
			}

			cand.score += float(numSame) / std::max(args.size(), desc.arguments.size());

			float relDesc = float(desc.id) / desc.count;
			float relCand = float(c) / candidates.size();

			cand.score /= 1 + std::abs(5 * (relDesc - relCand));
		}

		// sort them in ascending order
		std::sort(candidates.begin(), candidates.end());

		// TODO: better filter
		auto threshold = 0.5f;
		auto cmp = [](float threshold, const auto& cand) {
			return threshold < cand.score;
		};
		auto it = std::upper_bound(candidates.begin(), candidates.end(), threshold, cmp);
		candidates.erase(candidates.begin(), it);

		return candidates;
	};

	std::vector<std::vector<Candidate>> levels;
	levels.push_back(findCandidates(cb.commands, desc[0]));

	auto i = 1u;
	while(true) {
		if(levels.back().empty()) {
			levels.pop_back();
			if(i == 1u) {
				return nullptr;
			}

			--i;
			continue;
		}

		// get the best parent candidate
		auto cand = levels.back().back();
		levels.back().pop_back();

		// if we are in the last level: nice, just return the best
		// candidate we have found
		if(i == desc.size()) {
			return cand.command;
		}

		// otherwise: we must have a parent command.
		auto sectionCmd = dynamic_cast<SectionCommand*>(cand.command);
		dlg_assert(sectionCmd);

		// Find all children candidates, and push them to the stack,
		// go one level deeper
		auto cands = findCandidates(sectionCmd->children, desc[i]);
		levels.push_back(cands);
		++i;
	}
}

template<typename T>
void resourceRefButton(Gui& gui, T& resource) {
	if(ImGui::Button(name(resource).c_str())) {
		gui.selectResource(resource);
	}
}


template<typename H>
auto rawHandles(const std::vector<H*>& handles) {
	using VkH = decltype(handles[0]->handle);
	std::vector<VkH> ret;
	ret.reserve(handles.size());
	for(auto* h : handles) {
		ret.push_back(h->handle);
	}

	return ret;
}

void WaitEventsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkEvents = rawHandles(this->events);
	dev.dispatch.CmdWaitEvents(cb,
		vkEvents.size(), vkEvents.data(),
		this->srcStageMask, this->dstStageMask,
		this->memBarriers.size(), this->memBarriers.data(),
		this->bufBarriers.size(), this->bufBarriers.data(),
		this->imgBarriers.size(), this->imgBarriers.data());

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

	auto str = dlg::format("v {}[{}]", name(*buf.buffer), buf.offset);
	ret.push_back(str);
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundIndexBuffer& buf) {
	if(!buf.buffer) {
		return;
	}

	auto str = dlg::format("{}[{}]", name(*buf.buffer), buf.offset);
	ret.push_back(str);
	ret.push_back(vk::name(buf.type));
}

void addToArgumentsDesc(std::vector<std::string>& ret, const BoundDescriptorSet& set) {
	if(!set.ds) {
		return;
	}

	ret.push_back(name(*set.ds));
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
		ret.push_back(fuen::name(*val));
	} else if constexpr(validExpression<VkNameExpr, T>) {
		ret.push_back(vk::name(val));
	} else if constexpr(validExpression<ToStringExpr, T>) {
		ret.push_back(std::to_string(val));
	} else {
		static_assert(templatize<T>(false), "Invalid type");
	}
}

template<typename T>
void addToArgumentsDesc(std::vector<std::string>& ret, span<const T> values) {
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

std::vector<std::string> WaitEventsCmd::argumentsDesc() const {
	return createArgumentsDesc(events,
		vk::flagNames(VkPipelineStageFlagBits(srcStageMask)),
		vk::flagNames(VkPipelineStageFlagBits(dstStageMask)));
}

void BarrierCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPipelineBarrier(cb,
		this->dependencyFlags,
		this->srcStageMask, this->dstStageMask,
		this->memBarriers.size(), this->memBarriers.data(),
		this->bufBarriers.size(), this->bufBarriers.data(),
		this->imgBarriers.size(), this->imgBarriers.data());

}

std::vector<std::string> BarrierCmd::argumentsDesc() const {
	return createArgumentsDesc(
		vk::flagNames(VkPipelineStageFlagBits(srcStageMask)),
		vk::flagNames(VkPipelineStageFlagBits(dstStageMask)));
}

void BeginRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBeginRenderPass(cb, &this->info, this->subpassContents);
}

std::vector<std::string> BeginRenderPassCmd::argumentsDesc() const {
	return createArgumentsDesc(*rp, subpassContents);
}

void BeginRenderPassCmd::displayInspector(Gui& gui) const {
	resourceRefButton(gui, *fb);
	resourceRefButton(gui, *rp);
}

void NextSubpassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdNextSubpass(cb, this->subpassContents);
}

void EndRenderPassCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndRenderPass(cb);
}

void DrawCmdBase::displayGrahpicsState(Gui& gui, bool indices) const {
	if(indices) {
		dlg_assert(state.indices.buffer);
		imGuiText("Index Buffer: ");
		ImGui::SameLine();
		resourceRefButton(gui, *state.indices.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}, Type {}", state.indices.offset, vk::name(state.indices.type));
	}

	resourceRefButton(gui, *state.pipe);

	imGuiText("Verex buffers");
	for(auto& vertBuf : state.vertices) {
		if(!vertBuf.buffer) {
			imGuiText("null");
			continue;
		}

		resourceRefButton(gui, *vertBuf.buffer);
		ImGui::SameLine();
		imGuiText("Offset {}", vertBuf.offset);
	}

	imGuiText("Descriptors");
	for(auto& ds : state.descriptorSets) {
		if(!ds.ds) {
			imGuiText("null");
			continue;
		}

		resourceRefButton(gui, *ds.ds);
		// TODO: dynamic offsets
	}

	// TODO: push constants
}

void DrawCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

std::vector<std::string> DrawCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.vertices, state.descriptorSets,
		vertexCount, instanceCount, firstInstance, firstVertex);
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
	resourceRefButton(gui, *buffer);
	DrawCmdBase::displayGrahpicsState(gui, false);
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

// DrawIndirectCountCmd
void DrawIndirectCountCmd::record(const Device& dev, VkCommandBuffer cb) const {
	if(indexed) {
		dev.dispatch.CmdDrawIndexedIndirectCount(cb, buffer->handle, offset,
			countBuffer->handle, countBufferOffset, maxDrawCount, stride);
	} else {
		dev.dispatch.CmdDrawIndirectCount(cb, buffer->handle, offset,
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
	resourceRefButton(gui, *buffer);

	imGuiText("Count buffer:");
	ImGui::SameLine();
	resourceRefButton(gui, *countBuffer);

	DrawCmdBase::displayGrahpicsState(gui, false);
}

// BindVertexBuffersCmd
void BindVertexBuffersCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkbuffers = rawHandles(buffers);
	dev.dispatch.CmdBindVertexBuffers(cb, firstBinding,
		vkbuffers.size(), vkbuffers.data(), offsets.data());
}

void BindIndexBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindIndexBuffer(cb, buffer->handle, offset, indexType);
}

void BindDescriptorSetCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkds = rawHandles(sets);
	dev.dispatch.CmdBindDescriptorSets(cb, pipeBindPoint, pipeLayout->handle,
		firstSet, vkds.size(), vkds.data(),
		dynamicOffsets.size(), dynamicOffsets.data());
}

void DispatchCmdBase::displayComputeState(Gui& gui) const {
	resourceRefButton(gui, *state.pipe);

	imGuiText("Descriptors");
	for(auto& ds : state.descriptorSets) {
		if(!ds.ds) {
			imGuiText("null");
			continue;
		}

		resourceRefButton(gui, *ds.ds);
		// TODO: dynamic offsets
	}

	// TODO: push constants
}

void DispatchCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatch(cb, groupsX, groupsY, groupsZ);
}

std::vector<std::string> DispatchCmd::argumentsDesc() const {
	return createArgumentsDesc(state.pipe, state.descriptorSets, groupsX, groupsY, groupsZ);
}

void DispatchIndirectCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatchIndirect(cb, buffer->handle, offset);
}

void DispatchIndirectCmd::displayInspector(Gui& gui) const {
	// TODO: display effective dispatch command
	resourceRefButton(gui, *buffer);
	DispatchCmdBase::displayComputeState(gui);
}

std::vector<std::string> DispatchIndirectCmd::argumentsDesc() const {
	return createArgumentsDesc(*buffer, offset, state.pipe, state.descriptorSets);
}

void DispatchBaseCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdDispatchBase(cb, baseGroupX, baseGroupY, baseGroupZ,
		groupsX, groupsY, groupsZ);
}

std::vector<std::string> DispatchBaseCmd::argumentsDesc() const {
	return createArgumentsDesc(baseGroupX, baseGroupY, baseGroupZ,
		groupsX, groupsY, groupsZ);
}

void CopyImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		copies.size(), copies.data());
}

void CopyImageCmd::displayInspector(Gui& gui) const {
	resourceRefButton(gui, *src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	resourceRefButton(gui, *dst);

	// TODO: copies
}

void CopyBufferToImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBufferToImage(cb, src->handle, dst->handle,
		imgLayout, copies.size(), copies.data());
}

void CopyBufferToImageCmd::displayInspector(Gui& gui) const {
	resourceRefButton(gui, *src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	resourceRefButton(gui, *dst);

	// TODO: copies
}

void CopyImageToBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyImageToBuffer(cb, src->handle, imgLayout, dst->handle,
		copies.size(), copies.data());
}

void CopyImageToBufferCmd::displayInspector(Gui& gui) const {
	resourceRefButton(gui, *src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	resourceRefButton(gui, *dst);

	// TODO: copies
}

void BlitImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBlitImage(cb, src->handle, srcLayout, dst->handle, dstLayout,
		blits.size(), blits.data(), filter);
}

void BlitImageCmd::displayInspector(Gui& gui) const {
	resourceRefButton(gui, *src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	resourceRefButton(gui, *dst);

	// TODO: filter
	// TODO: blits
}

void ClearColorImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearColorImage(cb, dst->handle, imgLayout, &color,
		ranges.size(), ranges.data());
}

void ClearDepthStencilImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearDepthStencilImage(cb, dst->handle, imgLayout, &value,
		ranges.size(), ranges.data());
}

void ClearAttachmentCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdClearAttachments(cb, attachments.size(),
		attachments.data(), rects.size(), rects.data());
}

void ResolveImageCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResolveImage(cb, src->handle, srcLayout,
		dst->handle, dstLayout, regions.size(), regions.data());
}

void SetEventCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetEvent(cb, event->handle, stageMask);
}

void ResetEventCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResetEvent(cb, event->handle, stageMask);
}

void CopyBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyBuffer(cb, src->handle, dst->handle,
		regions.size(), regions.data());
}

void CopyBufferCmd::displayInspector(Gui& gui) const {
	resourceRefButton(gui, *src);
	ImGui::SameLine();
	imGuiText(" -> ");
	ImGui::SameLine();
	resourceRefButton(gui, *dst);

	// TODO: copies
}

void UpdateBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdUpdateBuffer(cb, dst->handle, offset, data.size(), data.data());
}

void FillBufferCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdFillBuffer(cb, dst->handle, offset, size, data);
}

void ExecuteCommandsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	auto vkcbs = rawHandles(secondaries);
	dev.dispatch.CmdExecuteCommands(cb, vkcbs.size(), vkcbs.data());
}

std::vector<std::string> ExecuteCommandsCmd::argumentsDesc() const {
	return createArgumentsDesc(secondaries);
}

const Command* ExecuteCommandsCmd::display(const Command* selected, TypeFlags typeFlags) const {
	auto flags = ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;
	if(this == selected) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	const Command* ret = nullptr;
	if(ImGui::TreeNodeEx(this, flags, "CmdExecuteCommands")) {
		if(ImGui::IsItemClicked()) {
			ret = this;
		}

		// The most common use case: one single command buffer is executed.
		// In that case we show a simpler UI
		if(secondaries.size() == 1) {
			auto* cmd = secondaries[0];
			auto reti = displayCommands(cmd->commands, selected, typeFlags);
			if(reti) {
				dlg_assert(!ret);
				ret = reti;
			}
		} else {
			for(auto* cmd : secondaries) {
				auto cname = name(*cmd);
				auto treeID = dlg::format("{}:{}", this, cname);
				if(ImGui::TreeNode(treeID.c_str(), "%s", cname.c_str())) {
					auto reti = displayCommands(cmd->commands, selected, typeFlags);
					if(reti) {
						dlg_assert(!ret);
						ret = reti;
					}

					ImGui::TreePop();
				}
			}
		}

		ImGui::TreePop();
	}

	return ret;
}

void ExecuteCommandsCmd::displayInspector(Gui& gui) const {
	for(auto* cb : secondaries) {
		resourceRefButton(gui, *cb);
	}
}

void BeginDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	VkDebugUtilsLabelEXT label {};
	label.pLabelName = name.c_str();
	std::memcpy(&label.color, this->color.data(), sizeof(label.color));
	dev.dispatch.CmdBeginDebugUtilsLabelEXT(cb, &label);
}

void EndDebugUtilsLabelCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndDebugUtilsLabelEXT(cb);
}

void BindPipelineCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBindPipeline(cb, bindPoint, pipe->handle);
}

void BindPipelineCmd::displayInspector(Gui& gui) const {
	dlg_assert(pipe->type == bindPoint);
	if(bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
		resourceRefButton(gui, *static_cast<ComputePipeline*>(pipe));
	} else if(bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		resourceRefButton(gui, *static_cast<GraphicsPipeline*>(pipe));
	}
}

void PushConstantsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdPushConstants(cb, layout->handle, stages, offset,
		values.size(), values.data());
}

void SetViewportCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetViewport(cb, first, viewports.size(), viewports.data());
}

void SetScissorCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdSetScissor(cb, first, scissors.size(), scissors.data());
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

void BeginQueryCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdBeginQuery(cb, pool->handle, query, flags);
}

void EndQueryCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdEndQuery(cb, pool->handle, query);
}

void ResetQueryPoolCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdResetQueryPool(cb, pool->handle, first, count);
}

void WriteTimestampCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdWriteTimestamp(cb, stage, pool->handle, query);
}

void CopyQueryPoolResultsCmd::record(const Device& dev, VkCommandBuffer cb) const {
	dev.dispatch.CmdCopyQueryPoolResults(cb, pool->handle, first, count,
		dstBuffer->handle, dstOffset, stride, flags);
}

} // namespace fuen