#pragma once

#include <fwd.hpp>
#include <nytl/span.hpp>
#include <util/linalloc.hpp>
#include <util/intrusive.hpp>
#include <util/debugMutex.hpp>
#include <threadContext.hpp>
#include <imageLayout.hpp>
#include <vk/vulkan.h>

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <list>
#include <cassert>

namespace vil {

enum class AttachmentType : u8 {
	color,
	input,
	depthStencil,
};

struct PushConstantData {
	span<std::byte> data; // full data
};

struct BoundDescriptorSet {
	// At record time, this points to the DescriptorPoolSetEntry object.
	// Since the descriptor set might get invalid later on, this
	// should not be accessed directly, unless we know for certain
	// that the descriptorSet must be valid, e.g. at submission time.
	// See CommandDescriptorSnapshot that maps these pointers
	// to the DescriptorState at submission time.
	// Unlike other handles, we don't unset these pointer even when the ds
	// is invalidated or destroyed since we need descriptor information about
	// potentially invalidated records for matching.
	// To detect whether the pointer is still valid outside of submission
	// time (e.g. when a new record is selected in the gui), we store
	// the pool and the dsID here.
	void* dsEntry {};
	DescriptorPool* dsPool {};
	u32 dsID {};

	span<u32> dynamicOffsets;
	PipelineLayout* layout {};
};

using BufferViewDescriptorRef = BufferView*;

struct DescriptorState {
	span<BoundDescriptorSet> descriptorSets;

	// TODO: we don't track this correctly atm.
	// important to do this, also fix in vil::bind(..., state) below then
	span<std::byte> pushDescriptors;

	// Will always re-allocate the descriptorSets span
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

// All information about the rendering section we are currently in.
// This can be a normal render pass, a render pass with a
// vk_khr_image_less_framebuffer (core in Vulkan 1.2) or a
// vk_khr_dynamic_rendering (core in Vulkan 1.3) section.
// That's why we don't store references to render pass or framebuffer here
// but have our own meta representation.
struct RenderPassInstanceState {
	span<ImageView*> colorAttachments;
	span<ImageView*> inputAttachments;
	ImageView* depthStencilAttachment {};

	// NOTE: add preserve attachments? resolve attachments?
};

struct GraphicsState : DescriptorState {
	BoundIndexBuffer indices {};
	span<BoundVertexBuffer> vertices;
	GraphicsPipeline* pipe {};

	// NOTE: knowing this here would be convenient in a couple of places
	// but we can't do to secondary commandbuffers not being required to
	// specify the framaebuffer they are rendering into on recording time.
	// const RenderPassInstanceState* rpi {};

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

// XXX: these must only be called while we can statically know that the record
// associated with the given state is still valid. Otherwise its references
// might be dangling or null (if unset).
void bind(Device&, VkCommandBuffer, const ComputeState&);

// Represents a mapping of descriptor set pointers, as present
// in Command(Record), to their respective states at submission time.
struct CommandDescriptorSnapshot {
	std::unordered_map<void*, IntrusivePtr<DescriptorSetCow>> states;
};

// Since command buffer recording can be a bottleneck, we use our
// linear allocator. We never use it in a scoped way but always just
// allocate (during recording) and then reset everything when the
// record is no longer needed.
template<typename T> using CommandAllocList = std::list<T,
	LinearUnscopedAllocator<T>>;
// NOTE: keep in mind that the vector data structure is problematic
//   with LinearAllocator when it has to reallocate often (since the
//   previous allocations are not freed for the lifetime of the CommandRecord).
template<typename T> using CommandAllocVector = std::vector<T,
	LinearUnscopedAllocator<T>>;
template<typename K, typename V,
		typename Hash = std::hash<K>,
		typename Equal = std::equal_to<K>> using CommandAllocHashMap =
	std::unordered_map<K, V, Hash, Equal,
		LinearUnscopedAllocator<std::pair<const K, V>>>;
template<typename K,
		typename Hash = std::hash<K>,
		typename Equal = std::equal_to<K>> using CommandAllocHashSet =
	std::unordered_set<K, Hash, Equal,
		LinearUnscopedAllocator<K>>;
constexpr struct ManualTag {} manualTag;

// NOTE: we don't need RefHandle.commands atm, so we comment it out.
// Until we use C++20 transparent lookup, it's a major performance impact (see useHandleImpl in cb.cpp)

// Links a 'DeviceHandle' to a 'CommandRecord'.
template<typename T>
struct RefHandle {
	explicit RefHandle(LinAllocator& alloc) noexcept /*: commands(alloc)*/ { (void) alloc; }

	// List of commands where the associated handle is used inside the
	// associated record.
	// CommandAllocList<Command*> commands;
	IntrusivePtr<T> handle;
};

template<typename T>
inline bool operator==(const RefHandle<T>& a, const RefHandle<T>& b) {
	return a.handle == b.handle;
}

template<typename T>
inline bool operator!=(const RefHandle<T>& a, const RefHandle<T>& b) {
	return a.handle != b.handle;
}

struct UsedImage : RefHandle<Image> {
	explicit UsedImage(LinAllocator& alloc) noexcept;
	CommandAllocVector<ImageSubresourceLayout> layoutChanges;
};

struct UsedDescriptorSet {
	explicit UsedDescriptorSet(LinAllocator& alloc) noexcept /*: commands(alloc)*/ { (void) alloc; }

	// Must not access directly, might have been destroyed.
	void* ds {};
	// CommandAllocList<Command*> commands; // the BindDescriptorSets commands
};

inline bool operator==(const UsedDescriptorSet& a, const UsedDescriptorSet& b) {
	return a.ds == b.ds;
}

inline bool operator!=(const UsedDescriptorSet& a, const UsedDescriptorSet& b) {
	return a.ds != b.ds;
}

struct RefHandleHash {
	template<typename T>
	size_t operator()(const RefHandle<T>& x) const {
		auto ptr = x.handle.get();
		return std::hash<std::remove_reference_t<decltype(ptr)>>{}(ptr);
	}
};

struct UsedDescriptorHash {
	size_t operator()(const UsedDescriptorSet& x) const {
		return std::hash<void*>{}(x.ds);
	}
};

template<typename T>
using UsedHandleSet = CommandAllocHashSet<RefHandle<T>, RefHandleHash>;

template<typename K>
auto find(UsedHandleSet<K>& used, K& elem) {
	// TODO: better, transparent find with c++20
	RefHandle<K> rh(ThreadContext::instance.linalloc_);
	rh.handle = IntrusivePtr<K>(acquireOwnership, &elem);
	auto it = used.find(rh);
	(void) rh.handle.release();
	return it;
}

// TODO: kinda hacky, only a template so we don't have to include stuff
template<typename Image>
auto find(CommandAllocHashSet<UsedImage, RefHandleHash>& used, Image& elem) {
	// TODO: better, transparent find with c++20
	UsedImage rh(ThreadContext::instance.linalloc_);
	rh.handle = IntrusivePtr<Image>(acquireOwnership, &elem);
	auto it = used.find(rh);
	(void) rh.handle.release();
	return it;
}

// TODO: kinda hacky, only a template so we don't have to include stuff
template<typename DescriptorSet>
inline auto find(CommandAllocHashSet<UsedDescriptorSet, UsedDescriptorHash>& used,
		DescriptorSet& ds) {
	// TODO: better, transparent find with c++20
	UsedDescriptorSet rh(ThreadContext::instance.linalloc_);
	rh.ds = &ds;
	auto it = used.find(rh);
	return it;
}

struct AccelStructCopy {
	AccelStruct* src;
	AccelStruct* dst;
};

// Represents the recorded state of a command buffer.
// We represent it as extra, reference-counted object so we can display
// old records as well.
struct CommandRecord {
	LinAllocator alloc;
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
	// Allocated in memory of CommandRecord.
	const char* cbName {};
	// whether the recording is finished (i.e. EndCommandBuffer called)
	bool finished {};
	// whether the record always needs a hook. Currently only true
	// for records containing CmdBuildAccelerationStructures(Indirect)
	// since we need to copy the data using for the acceleration structure.
	bool buildsAccelStructs {};
	// Whether the record has a broken label hierarchy.
	// Labels allow nesting in ways that mess with a strict hierarchy view.
	// Will display such records differently by default.
	bool brokenHierarchyLabels {};
	// The usageFlags passed to BeginCommandBuffer
	VkCommandBufferUsageFlags usageFlags {};

	// The hierachy of commands recording into this record.
	RootCommand* commands {};

	// DebugUtils labels can span across multiple records.
	// - numPopLables: the number of labels popped from the queue stack
	// - pushLabels: the labels to push to the queue after execution
	// For a command buffer that closes all label it opens and open all
	// label it closes, numPopLabels is 0 and pushLabels empty.
	u32 numPopLabels {};
	CommandAllocList<const char*> pushLables;

	CommandAllocList<AccelStructCopy> accelStructCopies;

	struct UsedHandles {
		// NOTE: change this when adding maps here!
		// Important for CmdExecuteCommands impl assert
		static constexpr auto handleTypeCount = 17u;

		UsedHandleSet<Buffer> buffers;
		UsedHandleSet<GraphicsPipeline> graphicsPipes;
		UsedHandleSet<ComputePipeline> computePipes;
		UsedHandleSet<RayTracingPipeline> rtPipes;
		UsedHandleSet<PipelineLayout> pipeLayouts;
		UsedHandleSet<DescriptorUpdateTemplate> dsuTemplates;
		UsedHandleSet<RenderPass> renderPasses;
		UsedHandleSet<Framebuffer> framebuffers;
		UsedHandleSet<QueryPool> queryPools;
		UsedHandleSet<ImageView> imageViews;
		UsedHandleSet<BufferView> bufferViews;
		UsedHandleSet<Sampler> samplers;
		UsedHandleSet<AccelStruct> accelStructs;
		UsedHandleSet<Event> events;
		UsedHandleSet<DescriptorPool> dsPools;

		CommandAllocHashSet<UsedDescriptorSet, UsedDescriptorHash> descriptorSets;
		CommandAllocHashSet<UsedImage, RefHandleHash> images;

		UsedHandles(LinAllocator& alloc);
	} used;

	// We have to keep the secondary records (via cmdExecuteCommands) alive
	// since the command buffers can be reused by the application and
	// we only reference the CommandRecord objects, don't copy them.
	CommandAllocList<IntrusivePtr<CommandRecord>> secondaries;

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
	explicit CommandRecord(ManualTag, Device* dev); // mainly for testing
	~CommandRecord();

	CommandRecord(CommandRecord&&) noexcept = delete;
	CommandRecord& operator=(CommandRecord&&) noexcept = delete;
};

// Checks if the given bound DescriptorSet is still valid.
// If so, returns it (and a lock making sure it's kept alive).
[[nodiscard]]
std::pair<DescriptorSet*, std::unique_lock<LockableBase(DebugMutex)>>
tryAccess(const BoundDescriptorSet&);

// Directly accesses the given descriptor set.
// Asserts that it can be accessed. This method must only be used if
// the validity of the BoundDescriptorSet is guaranteed, otherwise use
// tryAccessLocked.
// ONLY USE THIS DURING SUBMISSION
DescriptorSet& access(const BoundDescriptorSet&);

// Creates a snapshot of all descriptors relevant to the given command.
// For commands that aren't of type StateCmdBase (i.e. aren't
// draw/dispatch/traceRays) commands, returns an empty snapshot.
// Otherwise returns the map of all bound descriptors to their
// created/retrieved cows.
CommandDescriptorSnapshot snapshotRelevantDescriptors(Device&, const Command&);
CommandDescriptorSnapshot snapshotRelevantDescriptorsLocked(const Command&);
// Like snapshotRelevantDescriptors but for when the caller can guarantee
// that the descriptor sets are still valid. Faster.
CommandDescriptorSnapshot snapshotRelevantDescriptorsValidLocked(const Command&);

// Tries to find 'dst' in 'rec' and returns it full hierachy.
// Returns empty vector if it can't be found.
std::vector<const Command*> findHierarchy(const CommandRecord& rec, const Command& dst);

} // namespace vil
