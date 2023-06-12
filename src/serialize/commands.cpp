#include <serialize/serialize.hpp>
#include <serialize/util.hpp>
#include <serialize/internal.hpp>
#include <command/commands.hpp>
#include <command/alloc.hpp>
#include <util/util.hpp>

#include <image.hpp>
#include <buffer.hpp>
#include <rp.hpp>
#include <pipe.hpp>
#include <ds.hpp>
#include <sync.hpp>

namespace vil {

struct StateLoader;
struct StateSaver;

template<typename S>
struct BindState {
	const S* lastState {};
	span<std::byte> lastPCR {};
};

template<typename Slz, typename IO>
struct CommandSerializer {
	Slz& slz;
	IO& io;
	CommandRecord& rec;

	CommandSerializer(Slz& a, IO& b, CommandRecord& c) :
		slz(a), io(b), rec(c) {}

	BindState<GraphicsState> graphics {};
	BindState<ComputeState> compute {};
	BindState<RayTracingState> rt {};
};

struct CommandSaver : CommandSerializer<StateSaver, SaveBuf> {
	using CommandSerializer::CommandSerializer;
};
struct CommandLoader : CommandSerializer<StateLoader, LoadBuf> {
	using CommandSerializer::CommandSerializer;
	RecordBuilder& builder;

	CommandLoader(StateLoader& a, LoadBuf& b, RecordBuilder& c) :
		CommandSerializer(a, b, *c.record_), builder(c) {}
};

// saver
template<typename CmdType>
void fwdVisit(CommandSaver& slz, const CmdType& cmd) {
	auto off = slz.io.size();
	slz.slz.offsetToCommand[off] = &cmd;
	slz.slz.commandToOffset[&cmd] = off;

	write(slz.io, cmd.type());

	serializeMarker(slz.io, markerStartCommand + u64(cmd.type()), "Command");

	serialize(slz, slz.io, const_cast<CmdType&>(cmd));
}

// loader
template<typename Slz>
void saveCommand(Slz& slz, Command& cmd) {
	auto visitor = TemplateCommandVisitor([&slz](auto& cmd) {
		fwdVisit(slz, cmd);
	});
	cmd.visit(visitor);
}

// load command
template<typename Cmd>
struct CommandCreator {
	static Command& load(CommandLoader& loader) {
		auto& cmd = *loader.builder.record_->alloc.allocRaw<Cmd>();
		loader.builder.append(cmd);
		serialize(loader, loader.io, cmd);
		return cmd;
	}
};

constexpr auto creators = std::array{
	&CommandCreator<RootCommand>::load,
	&CommandCreator<WaitEventsCmd>::load,
	&CommandCreator<WaitEvents2Cmd>::load,
	&CommandCreator<BarrierCmd>::load,
	&CommandCreator<Barrier2Cmd>::load,
	&CommandCreator<BeginRenderPassCmd>::load,
	&CommandCreator<NextSubpassCmd>::load,
	&CommandCreator<FirstSubpassCmd>::load,
	&CommandCreator<EndRenderPassCmd>::load,
	&CommandCreator<DrawCmd>::load,
	&CommandCreator<DrawIndirectCmd>::load,
	&CommandCreator<DrawIndexedCmd>::load,
	&CommandCreator<DrawIndirectCountCmd>::load,
	&CommandCreator<DrawMultiCmd>::load,
	&CommandCreator<DrawMultiIndexedCmd>::load,
	&CommandCreator<BindVertexBuffersCmd>::load,
	&CommandCreator<BindIndexBufferCmd>::load,
	&CommandCreator<BindDescriptorSetCmd>::load,
	&CommandCreator<DispatchCmd>::load,
	&CommandCreator<DispatchIndirectCmd>::load,
	&CommandCreator<DispatchBaseCmd>::load,
	&CommandCreator<CopyImageCmd>::load,
	&CommandCreator<CopyBufferToImageCmd>::load,
	&CommandCreator<CopyImageToBufferCmd>::load,
	&CommandCreator<BlitImageCmd>::load,
	&CommandCreator<ResolveImageCmd>::load,
	&CommandCreator<CopyBufferCmd>::load,
	&CommandCreator<UpdateBufferCmd>::load,
	&CommandCreator<FillBufferCmd>::load,
	&CommandCreator<ClearColorImageCmd>::load,
	&CommandCreator<ClearDepthStencilImageCmd>::load,
	&CommandCreator<ClearAttachmentCmd>::load,
	&CommandCreator<SetEventCmd>::load,
	&CommandCreator<SetEvent2Cmd>::load,
	&CommandCreator<ResetEventCmd>::load,
	&CommandCreator<ExecuteCommandsChildCmd>::load,
	&CommandCreator<ExecuteCommandsCmd>::load,
	&CommandCreator<BeginDebugUtilsLabelCmd>::load,
	&CommandCreator<EndDebugUtilsLabelCmd>::load,
	&CommandCreator<InsertDebugUtilsLabelCmd>::load,
	&CommandCreator<BindPipelineCmd>::load,
	&CommandCreator<PushConstantsCmd>::load,
	&CommandCreator<SetViewportCmd>::load,
	&CommandCreator<SetScissorCmd>::load,
	&CommandCreator<SetLineWidthCmd>::load,
	&CommandCreator<SetDepthBiasCmd>::load,
	&CommandCreator<SetDepthBoundsCmd>::load,
	&CommandCreator<SetBlendConstantsCmd>::load,
	&CommandCreator<SetStencilCompareMaskCmd>::load,
	&CommandCreator<SetStencilWriteMaskCmd>::load,
	&CommandCreator<SetStencilReferenceCmd>::load,
	&CommandCreator<BeginQueryCmd>::load,
	&CommandCreator<EndQueryCmd>::load,
	&CommandCreator<ResetQueryPoolCmd>::load,
	&CommandCreator<WriteTimestampCmd>::load,
	&CommandCreator<CopyQueryPoolResultsCmd>::load,
	&CommandCreator<PushDescriptorSetCmd>::load,
	&CommandCreator<PushDescriptorSetWithTemplateCmd>::load,
	&CommandCreator<SetFragmentShadingRateCmd>::load,
	&CommandCreator<BeginConditionalRenderingCmd>::load,
	&CommandCreator<EndConditionalRenderingCmd>::load,
	&CommandCreator<SetLineStippleCmd>::load,
	&CommandCreator<SetCullModeCmd>::load,
	&CommandCreator<SetFrontFaceCmd>::load,
	&CommandCreator<SetPrimitiveTopologyCmd>::load,
	&CommandCreator<SetViewportWithCountCmd>::load,
	&CommandCreator<SetScissorWithCountCmd>::load,
	&CommandCreator<SetDepthTestEnableCmd>::load,
	&CommandCreator<SetDepthWriteEnableCmd>::load,
	&CommandCreator<SetDepthCompareOpCmd>::load,
	&CommandCreator<SetDepthBoundsTestEnableCmd>::load,
	&CommandCreator<SetStencilTestEnableCmd>::load,
	&CommandCreator<SetStencilOpCmd>::load,
	&CommandCreator<SetPatchControlPointsCmd>::load,
	&CommandCreator<SetRasterizerDiscardEnableCmd>::load,
	&CommandCreator<SetDepthBiasEnableCmd>::load,
	&CommandCreator<SetLogicOpCmd>::load,
	&CommandCreator<SetPrimitiveRestartEnableCmd>::load,
	&CommandCreator<SetSampleLocationsCmd>::load,
	&CommandCreator<SetDiscardRectangleCmd>::load,
	&CommandCreator<CopyAccelStructCmd>::load,
	&CommandCreator<CopyAccelStructToMemoryCmd>::load,
	&CommandCreator<CopyMemoryToAccelStructCmd>::load,
	&CommandCreator<WriteAccelStructsPropertiesCmd>::load,
	&CommandCreator<BuildAccelStructsCmd>::load,
	&CommandCreator<BuildAccelStructsIndirectCmd>::load,
	&CommandCreator<TraceRaysCmd>::load,
	&CommandCreator<TraceRaysIndirectCmd>::load,
	&CommandCreator<SetRayTracingPipelineStackSizeCmd>::load,
	&CommandCreator<BeginRenderPassCmd>::load,
	&CommandCreator<EndRenderingCmd>::load,
	&CommandCreator<SetVertexInputCmd>::load,
	&CommandCreator<SetColorWriteEnableCmd>::load,
};

Command& loadCommand(CommandLoader& loader) {
	// make sure the enum/array don't get out of sync
	// All things stochastic, so why not 'stochastic assertions'?!
	static_assert(creators[u32(CommandType::root)] ==
		&CommandCreator<RootCommand>::load);
	static_assert(creators[u32(CommandType::draw)] ==
		&CommandCreator<DrawCmd>::load);
	static_assert(creators[u32(CommandType::fillBuffer)] ==
		&CommandCreator<FillBufferCmd>::load);
	static_assert(creators[u32(CommandType::pushDescriptorSet)] ==
		&CommandCreator<PushDescriptorSetCmd>::load);
	static_assert(creators[u32(CommandType::traceRays)] ==
		&CommandCreator<TraceRaysCmd>::load);
	static_assert(creators[u32(CommandType::setColorWriteEnable)] ==
		&CommandCreator<SetColorWriteEnableCmd>::load);

	static_assert(u32(CommandType::count) == creators.size(),
		"Add creator for new command type in array!");

	auto off = loader.slz.recordOffset();

	auto cmdType = read<CommandType>(loader.io);
	dlg_assert_or(u32(cmdType) < creators.size(),
		throw std::invalid_argument("Invalid command type"));

	serializeMarker(loader.io, markerStartCommand + u64(cmdType), "Command");

	auto creator = creators[u32(cmdType)];
	auto& cmd = creator(loader);
	dlg_assert(cmd.type() == cmdType);

	loader.slz.offsetToCommand[off] = &cmd;
	loader.slz.commandToOffset[&cmd] = off;

	return cmd;
}

template<typename Slz, typename IO>
void serialize(Slz&, IO& io, CommandRecord& rec) {
	serialize(io, rec.queueFamily);
}

// cmd serialize util
template<typename Slz, typename IO, typename T>
void serializeCmdSpan(Slz& slz, IO& io, span<T>& span) {
	if constexpr(std::is_same_v<Slz, CommandLoader>) {
		auto size = read<u64>(io);
		span = alloc<T>(slz.rec, size);
		read(io, bytes(span)); // read directly
	} else {
		write<u64>(io, span.size());
		nytl::write(io, bytes(span));
	}
}

template<typename Slz, typename IO>
void serializeCmdString(Slz& slz, IO& io, const char*& str) {
	std::string_view sv(str ? str : "");
	serialize(io, sv);

	if constexpr(std::is_same_v<Slz, CommandLoader>) {
		str = copyString(slz.rec, sv);
	}
}

template<typename Slz, typename IO, typename T, typename Serializer>
void serializeCmdSpan(Slz& slz, IO& io, span<T>& span, Serializer&& func) {
	if constexpr(std::is_same_v<Slz, CommandLoader>) {
		auto size = read<u64>(io);
		span = alloc<T>(slz.rec, size);
		for(auto& val : span) {
			func(slz, io, val);
		}
	} else {
		write<u64>(io, span.size());
		for(auto& val : span) {
			func(slz, io, val);
		}
	}
}

template<typename Slz, typename IO, typename T>
void serializeCmdRefs(Slz& slz, IO& io, span<T>& span) {
	auto serializeRes = [](Slz& slz, auto& buf, auto& res) {
		serializeRef(slz.slz, buf, res);
	};
	serializeCmdSpan(slz, io, span, serializeRes);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, RayTracingState& state) {
	(void) slz;
	(void) io;
	(void) state;

	serializeRef(slz.slz, io, state.pipe);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, GraphicsState& state) {
	(void) slz;
	(void) io;
	(void) state;

	serializeRef(slz.slz, io, state.pipe);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, ComputeState& state) {
	(void) slz;
	(void) io;
	(void) state;

	serializeRef(slz.slz, io, state.pipe);
}

template<typename Slz, typename IO, typename Cmd, typename State>
void serializeState(Slz& slz, IO& io, Cmd& cmd, BindState<State>& bind) {
	// graphics state
	if constexpr(std::is_same_v<Slz, CommandLoader>) {
		auto newState = read<u8>(io);
		if(newState) {
			auto newState = allocRaw<State>(slz.rec);
			serialize(slz, io, const_cast<State&>(*newState));
			bind.lastState = newState;
		}
		cmd.state = bind.lastState;
	} else {
		auto newState = (cmd.state != bind.lastState);
		write<u8>(io, newState);
		if(newState) {
			serialize(slz, io, const_cast<State&>(*cmd.state));
			bind.lastState = cmd.state;
		}
	}

	// push constants
	if constexpr(std::is_same_v<Slz, CommandLoader>) {
		auto newPCR = read<u8>(io);
		if(newPCR) {
			auto size = read<u64>(io);
			bind.lastPCR = alloc<std::byte>(slz.rec, size);
			read(io, bind.lastPCR);
		}
		cmd.pushConstants.data = bind.lastPCR;
	} else {
		auto newPCR = cmd.pushConstants.data.data() != bind.lastPCR.data();
		write<u8>(io, newPCR);
		if(newPCR) {
			write<u64>(io, cmd.pushConstants.data.size());
			writeBytes(io, cmd.pushConstants.data); // TODO PERF: slow, add memcpy overload
			bind.lastPCR = cmd.pushConstants.data;
		}
	}
}

// specific command serialization
void loadChildren(CommandLoader& loader, LoadBuf& io) {
	auto childrenSize = read<u32>(io);
	if(childrenSize > io.buf.size()) {
		dlg_trace("childrenSize {}, io.buf.size() {}", childrenSize, io.buf.size());
		throw std::out_of_range("Serialization loading issue: Out of range (cmd children)");
	}

	auto currOff = loader.slz.recordOffset();
	while(currOff + childrenSize > loader.slz.recordOffset()) {
		loadCommand(loader);
	}

	dlg_assert(currOff + childrenSize == loader.slz.recordOffset());
}

void serialize(CommandLoader& loader, LoadBuf& io, SectionCommand& cmd) {
	loader.builder.beginSection(cmd);

	loadChildren(loader, io);

	dlg_assert(loader.builder.lastCommand_);
	loader.builder.endSection(loader.builder.lastCommand_);
}

void serialize(CommandSaver& saver, SaveBuf& io, SectionCommand& cmd) {
	// we will write the number of bytes consumed by child commands later on
	auto patchOff = io.size();
	write<u32>(io, 0u);

	auto child = cmd.children();
	while(child) {
		saveCommand(saver, *child);
		child = child->next;
	}

	auto numBytes = u32(io.size() - patchOff - 4);
	std::memcpy(&io[patchOff], &numBytes, sizeof(numBytes));
}

// draw commands
template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawCmdBase& cmd) {
	serializeState(slz, io, cmd, slz.graphics);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawCmd& cmd) {
	serialize(slz, io, static_cast<DrawCmdBase&>(cmd));

	serialize(io, cmd.vertexCount);
	serialize(io, cmd.instanceCount);
	serialize(io, cmd.firstVertex);
	serialize(io, cmd.firstInstance);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawIndirectCmd& cmd) {
	serialize(slz, io, static_cast<DrawCmdBase&>(cmd));

	serializeRef(slz.slz, io, cmd.buffer);
	serialize(io, cmd.offset);
	serialize(io, cmd.drawCount);
	serialize(io, cmd.stride);
	serialize(io, cmd.indexed);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawIndexedCmd& cmd) {
	serialize(slz, io, static_cast<DrawCmdBase&>(cmd));

	serialize(io, cmd.indexCount);
	serialize(io, cmd.instanceCount);
	serialize(io, cmd.firstIndex);
	serialize(io, cmd.vertexOffset);
	serialize(io, cmd.firstInstance);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawIndirectCountCmd& cmd) {
	serialize(slz, io, static_cast<DrawCmdBase&>(cmd));

	serializeRef(slz.slz, io, cmd.buffer);
	serialize(io, cmd.offset);
	serialize(io, cmd.maxDrawCount);
	serialize(io, cmd.stride);
	serializeRef(slz.slz, io, cmd.countBuffer);
	serialize(io, cmd.countBufferOffset);
	serialize(io, cmd.indexed);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawMultiCmd& cmd) {
	serialize(slz, io, static_cast<DrawCmdBase&>(cmd));

	serializeCmdSpan(slz, io, cmd.vertexInfos);
	serialize(io, cmd.instanceCount);
	serialize(io, cmd.firstInstance);
	serialize(io, cmd.stride);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DrawMultiIndexedCmd& cmd) {
	serialize(slz, io, static_cast<DrawCmdBase&>(cmd));

	serialize(io, cmd.instanceCount);
	serializeCmdSpan(slz, io, cmd.indexInfos);
	serialize(io, cmd.firstInstance);
	serialize(io, cmd.stride);
	serialize(io, cmd.vertexOffset);
}

// dispatch commands
template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DispatchCmdBase& cmd) {
	serializeState(slz, io, cmd, slz.compute);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DispatchCmd& cmd) {
	serialize(slz, io, static_cast<DispatchCmdBase&>(cmd));

	serialize(io, cmd.groupsX);
	serialize(io, cmd.groupsY);
	serialize(io, cmd.groupsZ);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DispatchIndirectCmd& cmd) {
	serialize(slz, io, static_cast<DispatchCmdBase&>(cmd));

	serializeRef(slz.slz, io, cmd.buffer);
	serialize(io, cmd.offset);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, DispatchBaseCmd& cmd) {
	serialize(slz, io, static_cast<DispatchCmdBase&>(cmd));

	serialize(io, cmd.baseGroupX);
	serialize(io, cmd.baseGroupY);
	serialize(io, cmd.baseGroupZ);
	serialize(io, cmd.groupsX);
	serialize(io, cmd.groupsY);
	serialize(io, cmd.groupsZ);
}

// rt
// TODO: figure out how to serialize device addresses.
template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, TraceRaysCmdBase& cmd) {
	serializeState(slz, io, cmd, slz.rt);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, TraceRaysCmd& cmd) {
	serialize(slz, io, static_cast<TraceRaysCmdBase&>(cmd));

	serialize(io, cmd.width);
	serialize(io, cmd.height);
	serialize(io, cmd.depth);
}

// transfer
// TODO: proper handling of pNext
// TODO: don't serialize pNext, sType of copy/blit objects.
//   find general way proper handle, serialize pNext.
template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, CopyImageCmd& cmd) {
	serializeRef(slz.slz, io, cmd.src);
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.srcLayout);
	serialize(io, cmd.dstLayout);
	serializeCmdSpan(slz, io, cmd.copies);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, CopyBufferToImageCmd& cmd) {
	serializeRef(slz.slz, io, cmd.src);
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.dstLayout);
	serializeCmdSpan(slz, io, cmd.copies);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, CopyImageToBufferCmd& cmd) {
	serializeRef(slz.slz, io, cmd.src);
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.srcLayout);
	serializeCmdSpan(slz, io, cmd.copies);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, BlitImageCmd& cmd) {
	serializeRef(slz.slz, io, cmd.src);
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.srcLayout);
	serialize(io, cmd.dstLayout);
	serializeCmdSpan(slz, io, cmd.blits);
	serialize(io, cmd.filter);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, ResolveImageCmd& cmd) {
	serializeRef(slz.slz, io, cmd.src);
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.srcLayout);
	serialize(io, cmd.dstLayout);
	serializeCmdSpan(slz, io, cmd.regions);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, CopyBufferCmd& cmd) {
	serializeRef(slz.slz, io, cmd.src);
	serializeRef(slz.slz, io, cmd.dst);
	serializeCmdSpan(slz, io, cmd.regions);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, UpdateBufferCmd& cmd) {
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.offset);
	serializeCmdSpan(slz, io, cmd.data);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, FillBufferCmd& cmd) {
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.offset);
	serialize(io, cmd.size);
	serialize(io, cmd.data);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, ClearColorImageCmd& cmd) {
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.color);
	serialize(io, cmd.dstLayout);
	serializeCmdSpan(slz, io, cmd.ranges);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, ClearDepthStencilImageCmd& cmd) {
	serializeRef(slz.slz, io, cmd.dst);
	serialize(io, cmd.value);
	serialize(io, cmd.dstLayout);
	serializeCmdSpan(slz, io, cmd.ranges);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, ClearAttachmentCmd& cmd) {
	serializeCmdSpan(slz, io, cmd.attachments);
	serializeCmdSpan(slz, io, cmd.rects);
}

// render passes
template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, BeginRenderPassCmd& cmd) {
	serialize(io, cmd.info.renderArea);
	serializeCmdSpan(slz, io, cmd.clearValues);
	serializeRef(slz.slz, io, cmd.rp);
	serializeRef(slz.slz, io, cmd.fb);
	serialize(io, cmd.subpassBeginInfo.contents);

	auto serializeAtt = [](Slz& slz, auto& buf, auto& att) {
		serializeRef(slz.slz, buf, att);
	};
	serializeCmdSpan(slz, io, cmd.attachments, serializeAtt);

	// serialize children
	serialize(slz, io, static_cast<SectionCommand&>(cmd));
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, SubpassCmd& cmd) {
	(void) slz;
	(void) io;
	serialize(io, cmd.subpassID);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, BeginRenderingCmd& cmd) {
	auto serializeAttachment = [](Slz& slz, auto& buf, auto& att) {
		serializeRef(slz.slz, buf, att.view);
		serialize(buf, att.imageLayout);
		serialize(buf, att.resolveMode);
		serializeRef(slz.slz, buf, att.resolveView);
		serialize(buf, att.resolveImageLayout);
		serialize(buf, att.loadOp);
		serialize(buf, att.storeOp);
		serialize(buf, att.clearValue);
	};

	serialize(io, cmd.layerCount);
	serialize(io, cmd.viewMask);
	serialize(io, cmd.flags);
	serialize(io, cmd.renderArea);

	serializeCmdSpan(slz, io, cmd.colorAttachments, serializeAttachment);
	serializeAttachment(slz, io, cmd.depthAttachment);
	serializeAttachment(slz, io, cmd.stencilAttachment);

	// serialize children
	serialize(slz, io, static_cast<SectionCommand&>(cmd));
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, BeginDebugUtilsLabelCmd& cmd) {
	serializeCmdString(slz, io, cmd.name);

	// serialize children
	serialize(slz, io, static_cast<SectionCommand&>(cmd));
}

// ExecuteCommands
void serialize(CommandLoader& loader, LoadBuf& io, ExecuteCommandsChildCmd& cmd) {
	auto recID = read<u64>(io);
	cmd.record_ = getRecord(loader.slz, recID).get();
	cmd.id_ = read<u32>(io);
}

void serialize(CommandSaver& saver, SaveBuf& io, ExecuteCommandsChildCmd& cmd) {
	auto recID = addNoFlush(saver.slz, *cmd.record_);
	write<u64>(io, recID);
	write<u32>(io, cmd.id_);
}

void serialize(CommandLoader& loader, LoadBuf& io, ExecuteCommandsCmd& cmd) {
	auto count = read<u32>(io);
	cmd.stats_.numChildSections = count;

	auto* last = static_cast<ExecuteCommandsChildCmd*>(nullptr);
	for(auto i = 0u; i < count; ++i) {
		auto& child = construct<ExecuteCommandsChildCmd>(loader.rec);
		serialize(loader, io, child);

		if(!last) {
			dlg_assert(!cmd.children_);
			cmd.children_ = &child;
		} else {
			dlg_assert(cmd.children_);
			last->next = &child;
			last->nextParent_ = &child;
		}

		last = &child;
	}
}

void serialize(CommandSaver& saver, SaveBuf& io, ExecuteCommandsCmd& cmd) {
	write<u32>(io, cmd.stats_.numChildSections); // num children
	[[maybe_unused]] auto count = 0u;
	auto* child = cmd.children_;
	while(child) {
		serialize(saver, io, *child);
		child = deriveCast<ExecuteCommandsChildCmd*>(child->next);
		++count;
	}

	dlg_assert(count == cmd.stats_.numChildSections);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, RootCommand& cmd) {
	serialize(slz, io, static_cast<SectionCommand&>(cmd));
}

// dummy for validExpression below
template<typename B> using SrcStageMaskMember = decltype(B::srcStageMask);

template<typename IO, typename BufBarrier>
void serializeBufBarrier(IO& io, BufBarrier& b) {
	serialize(io, b.srcQueueFamilyIndex);
	serialize(io, b.dstQueueFamilyIndex);
	serialize(io, b.srcAccessMask);
	serialize(io, b.dstAccessMask);
	serialize(io, b.offset);
	serialize(io, b.size);

	if constexpr(validExpression<SrcStageMaskMember, BufBarrier>) {
		serialize(io, b.srcStageMask);
		serialize(io, b.dstStageMask);
	}
}

template<typename IO, typename ImgBarrier>
void serializeImgBarrier(IO& io, ImgBarrier& b) {
	serialize(io, b.srcQueueFamilyIndex);
	serialize(io, b.dstQueueFamilyIndex);
	serialize(io, b.srcAccessMask);
	serialize(io, b.dstAccessMask);
	serialize(io, b.oldLayout);
	serialize(io, b.newLayout);
	serialize(io, b.subresourceRange);

	if constexpr(validExpression<SrcStageMaskMember, ImgBarrier>) {
		serialize(io, b.srcStageMask);
		serialize(io, b.dstStageMask);
	}
}

template<typename Slz, typename IO, typename BCmd>
void serializeBarrierCmd(Slz& slz, IO& io, BCmd& cmd) {
	serializeCmdRefs(slz, io, cmd.buffers);
	serializeCmdRefs(slz, io, cmd.images);

	auto serializeImgb = [](Slz&, auto& buf, auto& barrier) {
		serializeImgBarrier(buf, barrier);
	};
	auto serializeBufb = [](Slz&, auto& buf, auto& barrier) {
		serializeBufBarrier(buf, barrier);
	};
	auto serializeMemb = [](Slz&, auto& buf, auto& barrier) {
		serialize(buf, barrier.srcAccessMask);
		serialize(buf, barrier.dstAccessMask);
	};

	serializeCmdSpan(slz, io, cmd.memBarriers, serializeMemb);
	serializeCmdSpan(slz, io, cmd.bufBarriers, serializeBufb);
	serializeCmdSpan(slz, io, cmd.imgBarriers, serializeImgb);

	if constexpr(validExpression<SrcStageMaskMember, BCmd>) {
		serialize(io, cmd.srcStageMask);
		serialize(io, cmd.dstStageMask);
	}
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, BarrierCmd& cmd) {
	serializeBarrierCmd(slz, io, cmd);
	serialize(io, cmd.dependencyFlags);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, Barrier2Cmd& cmd) {
	serializeBarrierCmd(slz, io, cmd);
	serialize(io, cmd.flags);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, WaitEventsCmd& cmd) {
	serializeBarrierCmd(slz, io, cmd);
	serializeCmdRefs(slz, io, cmd.events);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, WaitEvents2Cmd& cmd) {
	serializeBarrierCmd(slz, io, cmd);
	serializeCmdRefs(slz, io, cmd.events);
}

template<typename Slz, typename IO>
void serialize(Slz& slz, IO& io, BindPipelineCmd& cmd) {
	serialize(io, cmd.bindPoint);
	serializeRef(slz.slz, io, cmd.pipe);
}

template<typename Cmd> constexpr bool NoSerialization =
	std::is_same_v<Cmd, EndDebugUtilsLabelCmd> ||
	std::is_same_v<Cmd, EndRenderPassCmd> ||
	std::is_same_v<Cmd, EndRenderingCmd>;

// fallback for all other commands
template<typename Slz, typename IO, typename Cmd>
void serialize(Slz& slz, IO& io, Cmd& cmd) {
	(void) slz;
	(void) io;
	(void) cmd;

	if(!NoSerialization<Cmd>) {
		dlg_trace("unimplemented: {}", cmd.nameDesc());
	}
}

// TODO: serializing of RootCommand currently not handled symmetrically

// entry points
void loadRecord(StateLoader& loader, IntrusivePtr<CommandRecord> recPtr, LoadBuf& io) {
	RecordBuilder builder;
	builder.reset(recPtr);

	auto& rec = *builder.record_;
	serialize(loader, io, rec);

	// load commands
	// start with the root command
	auto off = loader.recordOffset();

	CommandLoader cslz(loader, io, builder);
	auto type = read<CommandType>(io);
	dlg_assert_or(type == CommandType::root, throw std::invalid_argument("Expected root"));
	serializeMarker(io, markerStartCommand + u64(type), "Command");

	loader.offsetToCommand[off] = rec.commands;
	loader.commandToOffset[rec.commands] = off;

	loadChildren(cslz, cslz.io);
}

void saveRecord(StateSaver& saver, SaveBuf& io, CommandRecord& rec) {
	serialize(saver, io, rec);

	// save commands
	// serialize the root command as well for the section stats and
	//   to get the RootCommand id
	CommandSaver cslz(saver, io, rec);
	saveCommand(cslz, *rec.commands);
}

} // namespace vil

