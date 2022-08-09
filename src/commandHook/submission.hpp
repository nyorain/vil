#pragma once

#include <fwd.hpp>
#include <util/ownbuf.hpp>
#include <command/record.hpp>
#include <commandHook/state.hpp>

namespace vil {

struct CommandHookSubmission {
	CommandHookRecord* record {};
	CommandDescriptorSnapshot descriptorSnapshot {};

	CommandHookSubmission(CommandHookRecord&, Submission&,
		CommandDescriptorSnapshot descriptors);
	~CommandHookSubmission();

	// Called when the associated submission (passed again as parameter)
	// successfully completed execution on the device.
	void finish(Submission&);
	void finishAccelStructBuilds();
	void transmitTiming();
	void transmitIndirect();
};

} // namespace vil
