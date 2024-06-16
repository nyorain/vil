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

	// Called when the associated submission is activated, i.e. could
	// be started anytime on the device.
	// Called while device mutex is locked.
	void activate();

	// Called when the associated submission (passed again as parameter)
	// successfully completed execution on the device.
	// Called while device mutex is locked.
	void finish(Submission&);
	void transmitTiming();
	void transmitIndirect();

	void finishAccelStructBuilds();
};

} // namespace vil
