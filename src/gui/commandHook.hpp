#pragma once

#include <fwd.hpp>
#include <util/intrusive.hpp>
#include <gui/render.hpp>
#include <vk/vulkan.h>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <string>

namespace fuen {

struct BeginRenderPassCmd;

struct CopiedImage {
	Device* dev {};
	u32 refCount {};
	VkImage image {};
	VkImageView imageView {};
	VkDeviceMemory memory {};
	VkExtent3D extent {};
	VkImageAspectFlagBits aspect {};

	CopiedImage() = default;
	void init(Device& dev, VkFormat, const VkExtent3D&);
	~CopiedImage();
};

struct CopiedBuffer {
	OwnBuffer buffer {};
	void* map {};
	std::unique_ptr<std::byte[]> copy;

	CopiedBuffer() = default;
	void init(Device& dev, VkDeviceSize size);
	CopiedBuffer(CopiedBuffer&&) noexcept = default;
	CopiedBuffer& operator=(CopiedBuffer&&) noexcept = default;
	~CopiedBuffer() = default;

	void cpuCopy();
};

// TODO: we shouldn't store CopiedBuffer here i guess? just the cpu copy,
// move that out of CopiedBuffer itself?
struct CommandHookState {
	// We need a reference count here since this object is conceptually owned by
	// CommandRecord but may be read by the gui even when the record
	// was already destroyed (e.g. because it was replaced and all submissions
	// have finished).
	u32 refCount {};

	// Copy of the selected viewed descriptor set
	std::variant<std::monostate, CopiedImage, CopiedBuffer> dsCopy;
	u64 neededTime {}; // time needed for the given command

	// For indirect commands: holds a copy of the indirect command(s)
	CopiedBuffer indirectCopy {};

	// Only for draw commands
	std::vector<CopiedBuffer> vertexBufCopies {}; // draw cmd: Copy of all vertex buffers
	CopiedBuffer indexBufCopy {}; // draw cmd: Copy of index buffer
	CopiedImage attachmentCopy {}; // Copy of selected attachment

	// When a requested resource cannot be retrieved, this holds the reason.
	// TODO: kinda messy, should likely be provided per-resource
	std::string errorMessage;
};

// Commandbuffer hook that allows us to forward a modified version
// of this command buffer down the chain. Only called during submission,
// when the given CommandBuffer has a valid recording.
struct CommandHook {
public:
	struct DescriptorID {
		unsigned set {};
		unsigned binding {};
		unsigned elem {};
	};

	// Which operations/state copies to peform.
	// When updating e.g. the id of the ds to be copied, all existing
	// recordings have to be invalidated!
	// TODO: allow specifying offsets and max sizes. We currently
	//   have a rather arbirtrary static limit on buffer copy size.
	bool copyVertexBuffers {}; // could specify the needed subset in future
	bool copyIndexBuffers {};
	bool copyIndirectCmd {}; // always do that?
	std::optional<DescriptorID> copyDS;
	std::optional<unsigned> copyAttachment; // only for cmd inside renderpass
	bool queryTime {};

	// The last received copied state of a finished submission
	IntrusivePtr<CommandHookState> state;

	// TODO: shouldn't be here! See displayActionInspector.
	//   We need a better place to store this general state. CbGui?
	VkShaderStageFlagBits pcr {};

public:
	// Called from inside QueueSubmit with the command buffer the hook has
	// been installed for. Can therefore expect the command buffer to be
	// in executable state.
	// Can return the command buffer handle itself or a hooked one but
	// must return a valid command buffer.
	// Called while device mutex is locked.
	// Additionally has the possibility to return something in 'data' that
	// gets associated with the lifetime of the submission (i.e. is destroyed
	// when the submission is finished).
	VkCommandBuffer hook(CommandBuffer& hooked,
		PendingSubmission& subm,
		FinishPtr<CommandHookSubmission>& data);

	// Called when hook is removed from command buffer or command group.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	void finish() noexcept { delete this; }

	void invalidateRecordings();
	void invalidateData() { state = {}; }

	// automatically call invalidateRecordings and invalidateData
	void desc(std::vector<CommandDesc> desc);
	void unsetHookOps();

	~CommandHook();

private:
	friend struct CommandHookRecord;
	u32 counter_ {0};
	std::vector<CommandDesc> desc_ {};
	CommandHookRecord* records_ {}; // linked list
};

// Is kept alive only as long as the associated Record is referencing this
// (since it might resubmitted again, making this useful) or there are
// pending submission. When the record is invalidated, it no longer references
// the record.
// Since the record must stay alive and valid until all submissions have
// completed, we can assume the Record this hook was created for remains
// valid throughout its lifetime.
struct CommandHookRecord {
	CommandHook* hook {}; // Associated hook. Might be null if this was invalidated
	CommandRecord* record {}; // the record we hook. Always valid.
	u32 hookCounter {}; // hook->counter_ at creation time; for invalidation
	std::vector<Command*> hcommand; // hierachy of the hooked command

	// Currently we rely on there being at most 1 submission, so this is 0 or 1.
	// While there is a pending submission, this record is kept alive.
	u32 submissionCount {};

	// == Resources ==
	VkCommandBuffer cb {};

	// TODO: allocate resources from pool instead of giving each record
	// its entirely own set of resources (e.g. queryPool and images/buffers
	// in CommandHookState).
	VkQueryPool queryPool {};

	// When the viewed command is inside a render pass and we need to
	// perform transfer operations before/after it, we need to split
	// up the render pass.
	VkRenderPass rp0 {};
	VkRenderPass rp1 {};
	VkRenderPass rp2 {};

	IntrusivePtr<CommandHookState> state {};

	// Linked list of all records belonging to this->hook
	CommandHookRecord* next {};
	CommandHookRecord* prev {};

	CommandHookRecord(CommandHook& hook, CommandRecord& record,
		std::vector<Command*> hooked);
	~CommandHookRecord();

	struct RecordInfo {
		bool splitRenderPass {}; // whether we have to hook the renderpass
		u32 hookedSubpass {};
		const BeginRenderPassCmd* beginRenderPassCmd {};

		unsigned nextHookLevel {}; // on hcommand, hook hierarchy
	};

	void initState(RecordInfo&);

	void hookRecord(Command* cmdChain, RecordInfo);

	void copyDs(Command& bcmd, const RecordInfo&);
	void beforeDstOutsideRp(Command&, const RecordInfo&);
	void afterDstOutsideRp(Command&, const RecordInfo&);

	// Called when associated record is destroyed or hook replaced.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	void finish() noexcept;

	// TODO: kinda arbitrary, allow more. Configurable via settings?
	// In general, the problem is that we can't know the relevant
	// size for sub-allocated buffers. Theoretically, we could analyze
	// previous index/indirect data for this. Not sure if good idea.
	static constexpr auto maxBufCopySize = VkDeviceSize(16 * 1024);
};

struct CommandHookSubmission {
	CommandHookRecord* record {};

	CommandHookSubmission(CommandHookRecord&, PendingSubmission&);
	~CommandHookSubmission();

	// Called when the submission is finished.
	// Called while device mutex is locked.
	// Might delete itself (or decrement reference count or something).
	void finish() noexcept { delete this; }

	void transmitTiming();
	void transmitIndirect();
};

} // namespace fuen
