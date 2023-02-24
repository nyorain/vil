#include <serialize.hpp>
#include <command/commands.hpp>
#include <command/builder.hpp>
#include <command/alloc.hpp>
#include <util/util.hpp>

namespace vil {

// util
using nytl::read;
using nytl::write;

std::string_view readString(ReadBuf& buf) {
	auto size = read<u64>(buf);
	auto ptr = reinterpret_cast<const char*>(buf.data());
	auto ret = std::string_view(ptr, size);
	skip(buf, size);
	return ret;
}

void write(DynWriteBuf& buf, std::string_view str) {
	write<u64>(buf, str.size());
	write(buf, span<const char>(str.data(), str.size()));
}

// saving
struct StateSaver {
	std::unordered_map<u64, const Command*> offsetToCommand;
	std::unordered_map<const Command*, u64> commandToOffset;

	std::unordered_map<u64, const CommandRecord*> offsetToRecord;
	std::unordered_map<const CommandRecord*, u64> recordToOffset;

	DynWriteBuf buf;
};

struct CommandWriter : public CommandVisitor {
	DynWriteBuf& buf;
	StateSaver& svr;
	u32 baseOffset {};

	CommandWriter(DynWriteBuf& xbuf, StateSaver& xslz) : buf(xbuf), svr(xslz) {}

	void add(const Command& cmd) {
		// dlg_trace("{}", cmd.toString());
		auto off = baseOffset + buf.size();

		dlg_assert(svr.commandToOffset.find(&cmd) == svr.commandToOffset.end());
		svr.offsetToCommand[off] = &cmd;
		svr.commandToOffset[&cmd] = off;

		write(buf, cmd.type());
	}

	virtual void visit(const Command& cmd) {
		(void) cmd;
		// no-op
	}

	virtual void visit(const ParentCommand& cmd) {
		// we will write the number of bytes consumed by child commands
		// later on
		auto patchOff = buf.size();
		write<u32>(buf, 0u);

		auto child = cmd.children();
		while(child) {
			add(*child);
			child->visit(*this);
			child = child->next;
		}

		auto numBytes = u32(buf.size() - patchOff);
		std::memcpy(&buf[patchOff], &numBytes, sizeof(numBytes));
	}

	virtual void visit(const BeginDebugUtilsLabelCmd& cmd) {
		write(buf, std::string_view(cmd.name));
		visit(static_cast<const ParentCommand&>(cmd));
	}
};

StateSaverPtr createStateSaver() {
	auto ptr = StateSaverPtr{new StateSaver()};
	return ptr;
}

u64 add(StateSaver& slz, const CommandRecord& rec) {
	auto it = slz.recordToOffset.find(&rec);
	if(it != slz.recordToOffset.end()) {
		return it->second;
	}

	auto off = slz.buf.size();
	slz.offsetToRecord[off] = &rec;
	slz.recordToOffset[&rec] = off;

	// write record meta info
	write<u32>(slz.buf, rec.queueFamily);

	// write commands
	CommandWriter writer(slz.buf, slz);
	dlg_assert(rec.commands);
	auto& root = *rec.commands;
	writer.add(root);
	writer.visit(root);

	return off;
}

const CommandRecord* getRecord(const StateSaver& svr, u64 id) {
	auto it = svr.offsetToRecord.find(id);
	if(it == svr.offsetToRecord.end()) {
		dlg_trace("invalid record id");
		return nullptr;
	}

	return it->second;
}

u64 getID(const StateSaver& saver, const Command& cmd) {
	auto it = saver.commandToOffset.find(&cmd);
	if(it == saver.commandToOffset.end()) {
		dlg_error("Invalid command");
		return u64(-1);
	}

	return it->second;
}

ReadBuf getData(StateSaver& svr) {
	return svr.buf;
}

void destroy(StateSaver& saver) {
	delete &saver;
}

// loading
struct StateLoader {
	std::unordered_map<u64, Command*> offsetToCommand;
	std::unordered_map<const Command*, u64> commandToOffset;

	std::unordered_map<u64, IntrusivePtr<CommandRecord>> offsetToRecord;
	std::unordered_map<const CommandRecord*, u64> recordToOffset;

	ReadBuf buf;
	const std::byte* start {};

	u64 offset() const {
		return buf.data() - start;
	}
};

Command& loadCommand(StateLoader& loader, RecordBuilder& builder);

struct CommandLoader : public CommandVisitor {
	StateLoader& loader;
	RecordBuilder& builder;

	CommandLoader(StateLoader& xloader, RecordBuilder& xbuilder) :
		loader(xloader), builder(xbuilder) {}

	void visit(const Command& cmd) override {
		auto& dst = const_cast<Command&>(cmd);
		builder.append(dst);
	}

	void visit(const SectionCommand& cmd) override {
		auto& dst = const_cast<SectionCommand&>(cmd);
		builder.append(dst);
		builder.beginSection(dst);

		auto currOff = loader.offset();
		auto patchOff = read<u32>(loader.buf);

		while(currOff + patchOff > loader.offset()) {
			auto& child = loadCommand(loader, builder);
			builder.append(child);
		}

		dlg_assert(currOff + patchOff == loader.offset());

		dlg_assert(builder.lastCommand_);
		builder.endSection(builder.lastCommand_);
	}

	// 'execute commands' needs to be handled differently
	// ParentCommand but not a SectionCommand
	void visit(const ExecuteCommandsCmd& cmd) override {
		(void) cmd;
		dlg_warn("TODO");
	}

	void visit(const BeginDebugUtilsLabelCmd& cmd) override {
		auto& dst = const_cast<BeginDebugUtilsLabelCmd&>(cmd);
		auto str = readString(loader.buf);
		dst.name = copyString(*builder.record_, str);

		visit(static_cast<const SectionCommand&>(cmd));
	}
};

template<typename Cmd>
struct CommandCreator {
	static Command& load(StateLoader& loader, RecordBuilder& builder) {
		auto& ret = *builder.record_->alloc.allocRaw<Cmd>();

		CommandLoader cmdLoader(loader, builder);
		ret.visit(cmdLoader);

		return ret;
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

Command& loadCommand(StateLoader& loader, RecordBuilder& builder) {
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

	static_assert(u32(CommandType::count) == creators.size());

	auto off = loader.offset();

	auto cmdType = read<CommandType>(loader.buf);
	dlg_assert(u32(cmdType) < creators.size());
	auto creator = creators[u32(cmdType)];
	auto& cmd = creator(loader, builder);
	dlg_assert(cmd.type() == cmdType);

	loader.offsetToCommand[off] = &cmd;
	loader.commandToOffset[&cmd] = off;

	return cmd;
}

IntrusivePtr<CommandRecord> loadRecord(StateLoader& loader) {
	RecordBuilder builder;
	builder.reset(nullptr);

	auto ptr = builder.record_;
	auto& rec = *ptr;

	auto off = loader.offset();
	loader.offsetToRecord[off] = ptr;
	loader.recordToOffset[&rec] = off;

	rec.queueFamily = read<u32>(loader.buf);

	// load commands
	auto& rootCommand = loadCommand(loader, builder);
	dlg_assert(rootCommand.type() == CommandType::root);
	rec.commands = &static_cast<RootCommand&>(rootCommand);

	return ptr;
}

StateLoaderPtr createStateLoader(ReadBuf buf) {
	auto ptr = StateLoaderPtr{new StateLoader()};
	ptr->buf = buf;
	ptr->start = buf.data();

	while(!ptr->buf.empty()) {
		loadRecord(*ptr);
	}

	return ptr;
}

Command* getCommand(const StateLoader& ldr, u64 id) {
	auto it = ldr.offsetToCommand.find(id);
	if(it == ldr.offsetToCommand.end()) {
		dlg_assertm(id == u64(-1), "invalid command id {}", id);
		return nullptr;
	}

	return it->second;
}

IntrusivePtr<CommandRecord> getRecord(const StateLoader& loader, u64 id) {
	auto it = loader.offsetToRecord.find(id);
	if(it == loader.offsetToRecord.end()) {
		dlg_assertm(id == u64(-1), "invalid record id {}", id);
		return nullptr;
	}

	return it->second;
}

void destroy(StateLoader& loader) {
	delete &loader;
}

} // namespace vil
