#pragma once

#include <fwd.hpp>
#include <cow.hpp>
#include <util/ownbuf.hpp>

namespace vil {

struct DescriptorCopyOp {
	unsigned set {};
	unsigned binding {};
	unsigned elem {};
	bool before {}; // whether to copy before or after target command

	// If this is set to true, image data will be copied into a host-mapped
	// buffer as well. Useful to reading the data back on cpu instead
	// of displaying it.
	// Format of data in the buffer will be sampleFormat(imgFormat)
	bool imageAsBuffer {};
};

struct AttachmentCopyOp {
	unsigned id;
	AttachmentType type;
	bool before {}; // whether to copy before or after target command
};

// Collection of data we got out of a submission/command.
struct CommandHookState {
	struct CopiedDescriptor {
		std::variant<std::monostate, CopiedImage, OwnBuffer> data;
	};

	struct CopiedAttachment {
		CopiedImage data;
	};

	// We need a reference count here since this object is conceptually owned by
	// CommandRecord but may be read by the gui even when the record
	// was already destroyed (e.g. because it was replaced and all submissions
	// have finished).
	std::atomic<u32> refCount {};

	// Time needed for the given command.
	// Set to u64(-1) on error.
	u64 neededTime {u64(-1)};

	std::vector<CopiedDescriptor> copiedDescriptors;
	std::vector<CopiedAttachment> copiedAttachments;

	// For indirect commands: holds a copy of the indirect command(s)
	u32 indirectCommandCount {};
	OwnBuffer indirectCopy {};

	// Only for draw commands
	std::vector<OwnBuffer> vertexBufCopies {}; // draw cmd: Copy of all vertex buffers
	OwnBuffer indexBufCopy {}; // draw cmd: Copy of index buffer
	OwnBuffer transformFeedback {}; // draw cmd: position output of vertex stage

	// Only for transfer commands
	OwnBuffer transferBufCopy {};
	CopiedImage transferImgCopy {};

	CommandHookState();
	~CommandHookState();
};

// The format used for imageAsBuffer conversion.
// Implemented in cow.cpp
VkFormat sampleFormat(VkFormat src, VkImageAspectFlagBits aspect);

} // namespace vil
