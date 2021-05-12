#pragma once

#include <fwd.hpp>
#include <queue.hpp>
#include <command/alloc.hpp>
#include <util/span.hpp>
#include <util/intrusive.hpp>

#include <vector>
#include <unordered_map>
#include <utility>
#include <list>
#include <cassert>

namespace vil {

// IDEA: use something like this, track valid segments in push constant ranges
// IDEA: we should care about pipeline layouts for push constants.
// Not sure what the rules for disturbing them are though.
// struct PushConstantSegment {
// 	PushConstantSegment* next {};
// 	std::size_t size {};
// 	// std::byte data[size]; // following this
// };

struct PushConstantData {
	span<std::byte> data; // full data
};

struct BoundDescriptorSet {
	// At record time, this points to the DescriptorSet object.
	// But since the descriptor set might get invalid later on, this
	// should not be accessed directly, unless we know for certain
	// that the descriptorSet must be valid, e.g. at submission time.
	// See CommandDescriptorSnapshot that maps these pointers
	// to the DescriptorState at submission time.
	// Unlike other handles, we don't unset these pointer even when the ds
	// is invalidated or destroyed since we need descriptor information about
	// potentially invalidated records for matching.
	void* ds {};
	span<u32> dynamicOffsets;
	PipelineLayout* layout {}; // TODO: not sure if needed
};

struct ImageDescriptorRef {
	ImageView* imageView;
	Sampler* sampler; // even stored here if immutable in layout
	VkImageLayout layout {};
};

struct BufferDescriptorRef {
	Buffer* buffer;
	VkDeviceSize offset {};
	VkDeviceSize range {};
};

using BufferViewDescriptorRef = BufferView*;

struct DescriptorState {
	span<BoundDescriptorSet> descriptorSets;

	// TODO: we don't track this correctly atm.
	span<std::byte> pushDescriptors;

	void bind(CommandBuffer& cb, PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32> offsets);
};

struct BoundVertexBuffer {
	Buffer* buffer {};
	VkDeviceSize offset {};
};

struct BoundIndexBuffer {
	Buffer* buffer {};
	VkIndexType type {};
	VkDeviceSize offset {};
};

struct DynamicStateDepthBias {
	float constant {};
	float clamp {};
	float slope {};
};

struct RenderPassInstanceState {
	RenderPass* rp {};
	Framebuffer* fb {};
	unsigned subpass {};
};

struct GraphicsState : DescriptorState {
	BoundIndexBuffer indices {};
	span<BoundVertexBuffer> vertices;
	GraphicsPipeline* pipe {};

	// We can't store a BeginRenderPassCmd* here because that would not
	// work with secondary command buffers inside a render pass
	RenderPassInstanceState rpi {};

	struct StencilState {
		u32 writeMask {};
		u32 compareMask {};
		u32 reference {};
	};

	struct {
		span<VkViewport> viewports;
		span<VkRect2D> scissors;
		float lineWidth;
		DynamicStateDepthBias depthBias;
		std::array<float, 4> blendConstants;
		float depthBoundsMin;
		float depthBoundsMax;

		StencilState stencilFront;
		StencilState stencilBack;
	} dynamic {};
};

struct ComputeState : DescriptorState {
	ComputePipeline* pipe;
};

GraphicsState copy(CommandBuffer& cb, const GraphicsState& src);
ComputeState copy(CommandBuffer& cb, const ComputeState& src);

// We don't use shared pointers here, they are used in the
// commands referencing the handles.
struct UsedImage {
	UsedImage(Image& img, CommandRecord& rec) noexcept : image(&img), commands(rec) {}

	// UsedImage(const UsedImage&) = default;
	// UsedImage& operator=(const UsedImage&) = default;
	UsedImage(UsedImage&&) noexcept;
	UsedImage& operator=(UsedImage&&) noexcept;

	Image* image {};
	bool layoutChanged {};
	VkImageLayout finalLayout {}; // only valid/relevant when 'layoutChanged'
	CommandAllocList<Command*> commands;
};

// General definition covering all cases of handles not covered
// above.
struct UsedHandle {
	UsedHandle(DeviceHandle& h, CommandRecord& rec) noexcept : handle(&h), commands(rec) {}

	// UsedHandle(const UsedHandle&) = default;
	// UsedHandle& operator=(const UsedHandle&) = default;
	UsedHandle(UsedHandle&&) noexcept;
	UsedHandle& operator=(UsedHandle&&) noexcept;

	DeviceHandle* handle;
	CommandAllocList<Command*> commands;
};

struct CommandDescriptorSnapshot {
	std::unordered_map<void*, DescriptorSetStatePtr> states;
};

// Represents the recorded state of a command buffer.
// We represent it as extra, reference-counted object so we can display
// old records as well.
struct CommandRecord {
	Device* dev {};

	// We own those mem blocks, could even own them past command pool destruction.
	// Important this is the last object to be destroyed as other destructors
	// might still access that memory we free in this destructor.
	std::unique_ptr<CommandMemBlock, MemBlocksListDeleter> memBlocks {};
	std::size_t memBlockOffset {}; // offset in first (current) mem block

	// Might be null when this isn't the current command buffer recording.
	// Guaranteed to be valid during recording.
	CommandBuffer* cb {};
	// The id of this recording in the associated command buffers
	// Together with cb, uniquely identifies record.
	u32 recordID {};
	// The queue family this record was recorded for. Stored here separately
	// from CommandBuffer so that information is retained when cb is unset.
	u32 queueFamily {};
	// Name of commmand buffer in which this record originated.
	// Stored separately from cb so that information is retained when cb is unset.
	const char* cbName {};

	bool finished {}; // whether the recording is finished (i.e. EndCommandBuffer called)
	VkCommandBufferUsageFlags usageFlags {};

	// The hierachy of commands recording into this record.
	Command* commands {};

	// DebugUtils labels can span across multiple records.
	// - numPopLables: the number of labels popped from the queue stack
	// - pushLabels: the labels to push to the queue after execution
	// For a command buffer that closes all label it opens and open all
	// label it closes, numPopLabels is 0 and pushLabels empty.
	u32 numPopLabels {};
	CommandAllocList<const char*> pushLables;

	// IDEA: Should the key rather be Handle*? Also, maybe we rather
	// use a non-hash map here since rehashing might be a problem, especially
	// considering the memory waste through our custom allocator
	CommandAllocHashMap<VkImage, UsedImage> images;
	CommandAllocHashMap<u64, UsedHandle> handles;

	// We store all device handles referenced by this command buffer that
	// were destroyed since it was recorded so we can avoid deferencing
	// them in the command state.
	// TODO: we can change this back to an unordered set, don't ever
	// replace anymore.
	CommandAllocHashMap<DeviceHandle*, DeviceHandle*> invalidated;

	// We have to keep certain object alive that vulkan allows to be destroyed
	// after recording even if the command buffer is used in the future.
	CommandAllocList<IntrusivePtr<PipelineLayout>> pipeLayouts;
	// only needed for PushDescriptorSetWithTemplate
	CommandAllocList<IntrusivePtr<DescriptorUpdateTemplate>> dsUpdateTemplates;

	// We have to keep the secondary records (via cmdExecuteCommands) alive
	// since the command buffers can be reused by the application and
	// we only reference the CommandRecord objects, don't copy them.
	CommandAllocList<IntrusivePtr<CommandRecord>> secondaries;
	CommandBufferDesc desc;

	// descriptor state at the last submission of this command
	// TODO: really always copy this? This is kinda costly, we will likely
	// get away with only doing it in some cases
	CommandDescriptorSnapshot lastDescriptorState;

	// Ownership of this CommandRecord is shared: while generally it is
	// not needed anymore as soon as the associated CommandBuffer is
	// destroyed or a new record completed in it, it may be kept alive
	// as last representative of a command group or when viewed by
	// gui. We can't just transfer ownership in these cases in general
	// though since it may still be in use by command buffer.
	std::atomic<u32> refCount {0};

	// For command hooks: they can store data associated with this
	// recording here.
	FinishPtr<CommandHookRecord> hook;

	CommandRecord(CommandBuffer& cb);

	// NOTE: destructor expects that dev.mutex is locked.
	// This might seem weird but since access to CommandRecord references
	// is inherently synchronized by dev.mutex, it's the easiest way.
	~CommandRecord();
};

// Returns whether the given CommandRecord uses the given handle.
// Keep in mind that handles only used via descriptor sets won't appear here,
// one must iterate through all used descriptor sets to find them.
template<typename H>
bool uses(const CommandRecord& rec, const H& handle) {
	if constexpr(std::is_same_v<H, Image>) {
		return rec.images.find(handle.handle) != rec.images.end();
	} else {
		return rec.handles.find(handleToU64(vil::handle(handle))) != rec.handles.end();
	}
}

// Unsets all handles in record.destroyed in all of its commands and used
// handle entries. Must only be called while device mutex is locked
void replaceInvalidatedLocked(CommandRecord& record);

} // namespace vil
