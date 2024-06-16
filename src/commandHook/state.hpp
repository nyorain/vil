#pragma once

#include <fwd.hpp>
#include <util/ownbuf.hpp>
#include <vk/vulkan_core.h>
#include <accelStruct.hpp>
#include <variant>
#include <vector>
#include <optional>
#include <atomic>

namespace vil {

struct CopiedImageToBuffer {
	OwnBuffer buffer;
	VkFormat format; // format of the data
};

struct CopiedImage {
	Device* dev {};
	VkImage image {};
	VkDeviceMemory memory {};
	VkExtent3D extent {};
	u32 layerCount {};
	u32 levelCount {};
	VkImageAspectFlags aspectMask {};
	VkDeviceSize neededMemory {};
	VkFormat format {};

	CopiedImage() = default;
	[[nodiscard]] bool init(Device& dev, VkFormat, const VkExtent3D&,
		u32 layers, u32 levels, VkImageAspectFlags aspects, u32 srcQueueFam);
	~CopiedImage();

	VkImageSubresourceRange subresRange() const {
		return {aspectMask, 0, levelCount, 0, layerCount};
	}
};

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
	struct CapturedAccelStruct {
		IntrusivePtr<AccelStructState> tlas;
		std::unordered_map<u64, IntrusivePtr<AccelStructState>> blases;
	};

	struct CopiedDescriptor {
		DescriptorCopyOp op;
		std::variant<std::monostate,
			CopiedImage,
			OwnBuffer,
			CopiedImageToBuffer,
			CapturedAccelStruct> data;
	};

	struct CopiedAttachment {
		AttachmentCopyOp op;
		CopiedImage data;
	};

	struct CopiedTransferIO {
		OwnBuffer buf {};
		CopiedImage img {};
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
	CopiedTransferIO transferSrcBefore {};
	CopiedTransferIO transferSrcAfter {};
	CopiedTransferIO transferDstBefore {};
	CopiedTransferIO transferDstAfter {};

	CommandHookState();
	~CommandHookState();
};

const CommandHookState::CopiedDescriptor* findDsCopy(const CommandHookState&,
	unsigned setID, unsigned bindingID, unsigned elemID,
	std::optional<bool> before = std::nullopt,
	std::optional<bool> imageAsBuffer = std::nullopt);
const CommandHookState::CopiedAttachment* findAttachmentCopy(const CommandHookState&,
	AttachmentType type, unsigned id,
	std::optional<bool> before = std::nullopt);

} // namespace vil
