#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <vector>

namespace vil {

// Information about a single submission done during a swapchains frame duration.
struct FrameSubmission {
	Queue* queue {};
	SubmissionType type {};
	u64 submissionID {}; // global submission id
	std::vector<BindSparseSubmission> sparseBinds;
	std::vector<IntrusivePtr<CommandRecord>> submissions; // for command submission

	FrameSubmission();
	~FrameSubmission();
};

// All submissions done during a swapchains frame duration.
struct FrameSubmissions {
	// presentID (swapchain.presentCounter) associated with this frame.
	u64 presentID {};

	// global submission id (dev.submissionCounter) of the first
	// submission associated with this frame.
	u64 submissionStart {};

	// global submission id (dev.submissionCounter) of the last
	// submission associated with this frame.
	u64 submissionEnd {};

	std::vector<FrameSubmission> batches;
};

} // namespace vil
