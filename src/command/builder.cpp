#include <command/builder.hpp>
#include <command/commands.hpp>
#include <command/alloc.hpp>

#ifdef VIL_COMMAND_CALLSTACKS
	#include <backward/trace.hpp>
#endif // VIL_COMMAND_CALLSTACKS

namespace vil {

// util
void doReset(RecordBuilder& builder) {
	builder.record_->commands = &construct<RootCommand>(*builder.record_);
	builder.section_ = &construct<RecordBuilder::Section>(*builder.record_);
	builder.section_->cmd = builder.record_->commands;
	builder.lastCommand_ = nullptr;
}

// RecordBuilder
RecordBuilder::RecordBuilder(Device* dev) {
	reset(dev);
}

void RecordBuilder::reset(Device* dev) {
	record_ = IntrusivePtr<CommandRecord>(new CommandRecord(manualTag, dev));
	doReset(*this);
}

void RecordBuilder::reset(IntrusivePtr<CommandRecord> rec) {
	record_ = rec;
	doReset(*this);
}

RecordBuilder::RecordBuilder(CommandBuffer& cb) {
	reset(cb);
}

void RecordBuilder::reset(CommandBuffer& cb) {
	record_ = IntrusivePtr<CommandRecord>(new CommandRecord(cb));
	doReset(*this);
}

void RecordBuilder::appendParent(ParentCommand& cmd) {
	dlg_assert(section_);

	dlg_assert(!!section_->lastParentChild == !!section_->cmd->firstChildParent_);
	if(section_->lastParentChild) {
		section_->lastParentChild->nextParent_ = &cmd;
	} else {
		section_->cmd->firstChildParent_ = &cmd;
	}

	section_->lastParentChild = &cmd;
	++section_->cmd->stats_.numChildSections;
}

void RecordBuilder::beginSection(SectionCommand& cmd) {
	appendParent(cmd);

	if(section_->next) {
		// re-use a previously allocated section that isn't in use anymore
		dlg_assert(!section_->next->cmd);
		dlg_assert(section_->next->parent == section_);
		section_ = section_->next;
		section_->cmd = nullptr;
		section_->pop = false;
		section_->lastParentChild = nullptr;
	} else {
		auto nextSection = &construct<Section>(*record_);
		nextSection->parent = section_;
		section_->next = nextSection;
		section_ = nextSection;
	}

	section_->cmd = &cmd;
	lastCommand_ = nullptr; // will be set on first addCmd
}

void RecordBuilder::endSection(Command* cmd) {
	if(!section_->parent) {
		// We reached the root section.
		// Debug utils commands can span multiple command buffers, they
		// are only queue-local.
		// See docs/debug-utils-label-nesting.md
		dlg_assert(commandCast<EndDebugUtilsLabelCmd*>(cmd));
		++record_->numPopLabels;
		return;
	}

	lastCommand_ = section_->cmd;
	dlg_assert(!section_->pop); // we shouldn't be able to land here

	// reset it for future use
	section_->cmd = nullptr;
	section_->pop = false;

	// Don't unset section_->next, we can re-use the allocation
	// later on. We unset 'cmd' above to signal its unused (as debug check)
	section_ = section_->parent;

	// We pop the label sections here that were previously ended by
	// the application but not in the same nesting level they were created.
	while(section_->parent && section_->pop) {
		dlg_assert(commandCast<BeginDebugUtilsLabelCmd*>(section_->cmd));
		lastCommand_ = section_->cmd;

		// reset it for future use
		section_->cmd = nullptr;
		section_->pop = false;

		section_ = section_->parent;
	}
}

void RecordBuilder::append(Command& cmd) {
	dlg_assert(record_);
	dlg_assert(section_);

#ifdef VIL_COMMAND_CALLSTACKS
	// TODO: captureCmdStack does not really belong here,
	// don't want to access device. Should probably be passed
	// to RecordBuilder on construction or be a public attribute or smth
	if(record_->dev && record_->dev->captureCmdStack.load()) {
		cmd.stacktrace = backward::load_here(record_->alloc, 16u);
	}
#endif // VIL_COMMAND_CALLSTACKS

	// add to stats
	++section_->cmd->stats_.numTotalCommands;
	switch(cmd.category()) {
		case CommandCategory::draw:
			++section_->cmd->stats_.numDraws;
			break;
		case CommandCategory::dispatch:
			++section_->cmd->stats_.numDispatches;
			break;
		case CommandCategory::traceRays:
			++section_->cmd->stats_.numRayTraces;
			break;
		case CommandCategory::sync:
			++section_->cmd->stats_.numSyncCommands;
			break;
		case CommandCategory::transfer:
			++section_->cmd->stats_.numTransfers;
			break;
		default:
			break;
	}

	// append
	if(lastCommand_) {
		dlg_assert(record_->commands);
		lastCommand_->next = &cmd;
	} else {
		dlg_assert(record_->commands);
		dlg_assert(section_->cmd);
		dlg_assert(!section_->cmd->children_);
		section_->cmd->children_ = &cmd;
	}

	lastCommand_ = &cmd;
}

std::vector<const Command*> RecordBuilder::lastCommand() const {
	std::vector<const Command*> ret;
	ret.push_back(lastCommand_);

	auto section = section_;
	if(section->cmd == lastCommand_) {
		section = section->parent;
	}

	while(section) {
		ret.push_back(section->cmd);
		section = section->parent;
	}

	std::reverse(ret.begin(), ret.end());
	return ret;
}

} // namespace vil
