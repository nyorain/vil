#pragma once

#include <fwd.hpp>
#include <queue.hpp>
#include <util/span.hpp>
#include <util/intrusive.hpp>

#include <vector>
#include <unordered_map>
#include <utility>
#include <list>
#include <cassert>

namespace vil {

// Free-form of CommandBuffer::allocate
std::byte* allocate(CommandRecord&, std::size_t size, std::size_t alignment);
std::byte* allocate(CommandBuffer&, std::size_t size, std::size_t alignment);

template<typename T, typename... Args>
T& allocate(CommandBuffer& cb, Args&&... args) {
	auto* raw = allocate(cb, sizeof(T), alignof(T));
	return *(new(raw) T(std::forward<Args>(args)...));
}

template<typename T>
span<T> allocSpan(CommandBuffer& cb, std::size_t count) {
	if(count == 0) {
		return {};
	}

	auto* raw = allocate(cb, sizeof(T) * count, alignof(T));
	auto* arr = new(raw) T[count];
	return span<T>(arr, count);
}

template<typename T>
span<std::remove_const_t<T>> copySpan(CommandBuffer& cb, T* data, std::size_t count) {
	auto span = allocSpan<std::remove_const_t<T>>(cb, count);
	std::copy(data, data + count, span.data());
	return span;
}

template<typename T>
span<std::remove_const_t<T>> copySpan(CommandBuffer& cb, span<T> data) {
	return copySpan(cb, data.data(), data.size());
}

inline const char* copyString(CommandBuffer& cb, std::string_view src) {
	auto dst = allocSpan<char>(cb, src.size() + 1);
	std::copy(src.begin(), src.end(), dst.data());
	dst[src.size()] = 0;
	return dst.data();
}

// Allocates the memory from the command buffer.
void copyChainInPlace(CommandBuffer& cb, const void*& pNext);
[[nodiscard]] const void* copyChain(CommandBuffer& cb, const void* pNext);

template<typename T>
struct CommandAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	CommandRecord* rec;

	CommandAllocator(CommandRecord& xrec) noexcept : rec(&xrec) {}

	template<typename O>
	CommandAllocator(const CommandAllocator<O>& rhs) noexcept : rec(rhs.rec) {}

	template<typename O>
	CommandAllocator& operator=(const CommandAllocator<O>& rhs) noexcept {
		this->rec = rhs.rec;
		return *this;
	}

	T* allocate(std::size_t n) {
		assert(rec);
		auto* ptr = vil::allocate(*rec, n * sizeof(T), alignof(T));
        // TODO: strictly speaking we need the first line but it doesn't compile
        // under msvc for non-default-constructibe T
		// return new(ptr) T[n]; // creates the array but not the objects
        return reinterpret_cast<T*>(ptr);
	}

	void deallocate(T*, std::size_t) const noexcept {}
};

template<typename T>
bool operator==(const CommandAllocator<T>& a, const CommandAllocator<T>& b) noexcept {
	return a.rec == b.rec;
}

template<typename T>
bool operator!=(const CommandAllocator<T>& a, const CommandAllocator<T>& b) noexcept {
	return a.rec != b.rec;
}

// template<typename T> using CommandAllocPageVector = PageVector<T, CommandAllocator<T>>;
template<typename T> using CommandAllocList = std::list<T, CommandAllocator<T>>;
template<typename K, typename V> using CommandAllocHashMap = std::unordered_map<K, V,
	std::hash<K>, std::equal_to<K>, CommandAllocator<std::pair<const K, V>>>;
// template<typename K, typename V> using CommandMap = std::map<K, V,
//     std::less<K>, CommandAllocator<std::pair<const K, V>>>;

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
	DescriptorSet* ds {};
	PipelineLayout* layout {};
	span<u32> dynamicOffsets;
};

struct DescriptorState {
	span<BoundDescriptorSet> descriptorSets;

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

struct CommandMemBlock {
	std::atomic<CommandMemBlock*> next {};
	std::size_t size {};
	// std::byte block[size]; // following 'this' in memory
};

void freeBlocks(CommandMemBlock* memBlocks);
void returnBlocks(CommandPool& pool, CommandMemBlock* blocks);

struct MemBlockDeleter {
	// Set to null when pool is destroyed.
	// Remembered separately from cb so that this can return the allocated
	// memory blocks on destruction even if the command buffer this comes
	// from was already freed.
	// Synced via device mutex.
	Device* dev {};
	CommandPool* pool {};
	void operator()(CommandMemBlock* blocks);
};

// Represents the recorded state of a command buffer.
// We represent it as extra, reference-counted object so we can display
// old records as well.
// TODO: we could store the name of the command buffer this record originated
// from (if any) for better display in gui, e.g. when executed as secondary
// command buffer.
struct CommandRecord {
	// We own those mem blocks, could even own them past command pool destruction.
	// Important this is the last object to be destroyed other destructors
	// still access that memory.
	std::unique_ptr<CommandMemBlock, MemBlockDeleter> memBlocks {};
	std::size_t memBlockOffset {}; // offset in first (current) mem block

	// Might be null when this isn't the current command buffer recording.
	// Guaranteed to be valid during recording.
	CommandBuffer* cb {};
	// The id of this recording in the associated command buffers
	// Together with cb, uniquely identifies record.
	u32 recordID {};
	// The queue family this record was recorded for. Stored here separately
	// from CommandBuffer so that information is retained after cb destruction.
	u32 queueFamily {};

	bool finished {}; // whether the recording is finished (i.e. EndCommandBuffer called)
	VkCommandBufferUsageFlags usageFlags {};
	Command* commands {};

	// DebugUtils labels can span across multiple records.
	// - numPopLables: the number of labels popped from the queue stack
	// - pushLabels: the labels to push to the queue after execution
	// For a command buffer that closes all label it opens and open all
	// label it closes, numPopLabels is 0 and pushLabels empty.
	u32 numPopLabels {};
	std::vector<const char*> pushLables {}; // PERF: use pool memory

	// IDEA: Should the key rather be Handle*?
	CommandAllocHashMap<VkImage, UsedImage> images;
	CommandAllocHashMap<u64, UsedHandle> handles;

	// We store all device handles referenced by this command buffer that
	// were destroyed since it was recorded so we can avoid deferencing
	// them in the command state.
	// PERF: use pool memory here, too? could use alternate allocation function
	// that just allocates blocks on its own (but still returns them to pool
	// in the end?).
	std::unordered_set<DeviceHandle*> destroyed;

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
	CommandBufferGroup* group {};

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

	// Allocates a chunk of memory from the given command record, will use the
	// internal CommandPool memory allocator. The memory can not be freed in
	// any way, it will simply be reset when the record is destroyed (destructors
	// of non-trivial types inside the memory must be called before that!).
	// Only allowed to call in recording state (i.e. while record is not finished).
	std::byte* allocate(std::size_t size, std::size_t alignment);

	template<typename H>
	bool uses(const H& handle) const {
		if constexpr(std::is_same_v<H, Image>) {
			return images.find(handle.handle) != images.end();
		} else {
			return handles.find(handleToU64(vil::handle(handle))) != handles.end();
		}
	}

	Device& device() const { return *memBlocks.get_deleter().dev; }

	CommandRecord(CommandBuffer& cb);
	~CommandRecord();
};

// Unsets all handles in record.destroyed in all of its commands and used
// handle entries. Must only be called while device mutex is locked
void unsetDestroyedLocked(CommandRecord& record);

} // namespace vil
