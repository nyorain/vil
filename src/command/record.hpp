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
	PipelineLayout* layout {};
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
	// important to do this, also fix in vil::bind(..., state) below then
	span<std::byte> pushDescriptors;

	void bind(CommandBuffer& cb, PipelineLayout& layout, u32 firstSet,
		span<DescriptorSet* const> sets, span<const u32> offsets);
};

struct BoundVertexBuffer {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkDeviceSize size {}; // might be 0 for unknown
	VkDeviceSize stride {}; // might be 0
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
	unsigned subpass {};

	// NOTE: we store the attachments here instead of the framebuffer
	// to support VK_KHR_imageless_framebuffer (core in Vulkan 1.2).
	// Keep in mind that this may be empty for secondary command buffers,
	// even if they execute draw commands (the framebuffer parameter in the
	// inherited info for secondary cbs is optional).
	span<ImageView*> attachments;
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

struct RayTracingState : DescriptorState {
	RayTracingPipeline* pipe;
};

GraphicsState copy(CommandBuffer& cb, const GraphicsState& src);
ComputeState copy(CommandBuffer& cb, const ComputeState& src);
RayTracingState copy(CommandBuffer& cb, const RayTracingState& src);

// XXX: these must only be called while we can statically know that the record
// associated with the given state is still valid. Otherwise its references
// might be dangling or null (if unset).
void bind(Device&, VkCommandBuffer, const ComputeState&);

struct UsedHandle;

// Represents a mapping of descriptor set pointers, as present
// in Command(Record), to their respective states at submission time.
struct CommandDescriptorSnapshot {
	std::unordered_map<void*, IntrusivePtr<DescriptorSetCow>> states;
};

// Represents the recorded state of a command buffer.
// We represent it as extra, reference-counted object so we can display
// old records as well.
struct CommandRecord : CommandRecordMemory {
	Device* dev {};

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
	// whether the recording is finished (i.e. EndCommandBuffer called)
	bool finished {};
	// whether the record always needs a hook. Currently only true
	// for records containing CmdBuildAccelerationStructures(Indirect)
	// since we need to copy the data using for the acceleration structure.
	bool buildsAccelStructs {};

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

	// TODO: maybe we rather use a non-hash map here since rehashing
	// might be a problem, especially considering the memory waste through
	// our custom allocator
	CommandAllocHashMap<DeviceHandle*, UsedHandle*> handles;

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

	// Ownership of this CommandRecord is shared: while generally it is
	// not needed anymore as soon as the associated CommandBuffer is
	// destroyed or a new record completed in it, it may be kept alive
	// as last representative of a command group or when viewed by
	// gui. We can't just transfer ownership in these cases in general
	// though since it may still be in use by command buffer.
	std::atomic<u32> refCount {0};

	// For CommandHook: can store hooked versions of this record here.
	std::vector<FinishPtr<CommandHookRecord>> hookRecords;

	CommandRecord(CommandBuffer& cb);
	~CommandRecord();
};

// Links a 'DeviceHandle' to a 'CommandRecord'.
struct UsedHandle {
	UsedHandle(CommandRecord& rec) noexcept : commands(rec), record(&rec) {}

	// List of commands where the associated handle is used inside the
	// associated record.
	CommandAllocList<Command*> commands;

	// Links forming a linked list over all UsedHandle structs
	// for the associated handle. That's why we store
	// the associated record as well.
	UsedHandle* next {};
	UsedHandle* prev {};
	CommandRecord* record {};
};

struct UsedImage : UsedHandle {
	using UsedHandle::UsedHandle;
	bool layoutChanged {};
	VkImageLayout finalLayout {}; // only valid/relevant when 'layoutChanged'
};

// Returns whether the given CommandRecord uses the given handle.
// Keep in mind that handles only used via descriptor sets won't appear here,
// one must iterate through all used descriptor sets to find them.
template<typename H>
bool uses(const CommandRecord& rec, const H& handle) {
	if(rec.invalidated.find(&handle) != rec.invalidated.end()) {
		return false;
	}
	return rec.handles.find(&handle) != rec.handles.end();
}

// Unsets all handles in record.destroyed in all of its commands and used
// handle entries. Must only be called while device mutex is locked
void replaceInvalidatedLocked(CommandRecord& record);

} // namespace vil
