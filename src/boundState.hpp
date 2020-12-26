#pragma once

#include <fwd.hpp>
#include <device.hpp>
#include <pv.hpp>
#include <vector>

namespace fuen {

/*
struct PushConstantData {
	span<std::byte> data;
	span<std::pair<VkDeviceSize, VkDeviceSize>> ranges; // (offset, size)[]
};

using PushConstantMap = std::unordered_map<VkShaderStageFlagBits, PushConstantData>;
*/

struct BoundDescriptorSet {
	DescriptorSet* ds {};
	PipelineLayout* layout {};
	span<u32> dynamicOffsets;
};

struct DescriptorState {
	span<BoundDescriptorSet> descriptorSets;

	// TODO: fix with CommandAllocator
	// PushConstantMap pushConstants;

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
	float constant;
	float clamp;
	float slope;
};

struct GraphicsState : DescriptorState {
	BoundIndexBuffer indices;
	span<BoundVertexBuffer> vertices;
	GraphicsPipeline* pipe;
	RenderPass* rp;

	struct StencilState {
		u32 writeMask;
		u32 compareMask;
		u32 reference;
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
	} dynamic;
};

struct ComputeState : DescriptorState {
	ComputePipeline* pipe;
};

GraphicsState copy(CommandBuffer& cb, const GraphicsState& src);
ComputeState copy(CommandBuffer& cb, const ComputeState& src);

// Free-form of CommandBuffer::allocate
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

template<typename T>
struct CommandAllocator {
	using is_always_equal = std::false_type;
	using value_type = T;

	CommandBuffer* cb;

#ifdef DEBUG
	u32 allocated {};
#endif // DEBUG

	CommandAllocator(CommandBuffer& xcb) noexcept : cb(&xcb) {}

	template<typename O>
	CommandAllocator(const CommandAllocator<O>& rhs) noexcept : cb(rhs.cb) {}

	template<typename O>
	CommandAllocator& operator=(const CommandAllocator<O>& rhs) noexcept {
		this->cb = rhs.cb;
		return *this;
	}

	T* allocate(std::size_t n) {
		dlg_assert(cb);
		auto* ptr = fuen::allocate(*cb, n * sizeof(T), alignof(T));
        // TODO: strictly speaking we need the first line but it doesn't compile
        // under msvc for non-default-constructibe T
		// return new(ptr) T[n]; // creates the array but not the objects
        return reinterpret_cast<T*>(ptr);
	}

	void deallocate(T*, std::size_t) const noexcept {}
};

template<typename T>
bool operator==(const CommandAllocator<T>& a, const CommandAllocator<T>& b) noexcept {
	return a.cb == b.cb;
}

template<typename T>
bool operator!=(const CommandAllocator<T>& a, const CommandAllocator<T>& b) noexcept {
	return a.cb != b.cb;
}

template<typename T> using CommandVector = PageVector<T, CommandAllocator<T>>;
template<typename K, typename V> using CommandMap = std::unordered_map<K, V,
	std::hash<K>, std::equal_to<K>, CommandAllocator<std::pair<const K, V>>>;
// template<typename K, typename V> using CommandMap = std::map<K, V,
//     std::less<K>, CommandAllocator<std::pair<const K, V>>>;


// We don't use shared pointers here, they are used in the
// commands referencing the handles.
struct UsedImage {
	UsedImage(Image& img, CommandBuffer& cb) noexcept : image(&img), commands(cb) {}
	UsedImage(UsedImage&&) noexcept = default;
	UsedImage& operator=(UsedImage&&) noexcept = default;

	Image* image {};
	bool layoutChanged {};
	VkImageLayout finalLayout {}; // only valid/relevant when 'layoutChanged'
	CommandVector<Command*> commands;
};

// General definition covering all cases of handles not covered
// above.
struct UsedHandle {
	UsedHandle(DeviceHandle& h, CommandBuffer& cb) noexcept : handle(&h), commands(cb) {}
	UsedHandle(UsedHandle&&) noexcept = default;
	UsedHandle& operator=(UsedHandle&&) noexcept = default;

	DeviceHandle* handle;
	CommandVector<Command*> commands;
};

struct CommandMemBlock {
	std::atomic<CommandMemBlock*> next {};
	std::size_t size {};
	// std::byte block[size]; // following 'this' in memory
};

void freeBlocks(const CommandMemBlock* memBlocks);
void returnBlocks(CommandPool& pool, const CommandMemBlock* blocks);

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

struct CommandBufferRecord {
	// We own those mem blocks, could even own them past command pool destruction.
	// Important this is the last object to be destroyed other destructors
	// still access that memory.
	std::unique_ptr<CommandMemBlock, MemBlockDeleter> memBlocks {};

	// Might be null when cb is destroyed.
	// Guaranteed to be valid during recording though.
	CommandBuffer* cb {};
	// The id of this recording in the associated command buffers
	// Together with cb, uniquely identifies record.
	u32 recordID {};

	Command* commands {};

	CommandMap<VkImage, UsedImage> images;
	CommandMap<u64, UsedHandle> handles;

	// We store all device handles referenced by this command buffer that
	// were destroyed since it was recorded so we can avoid deferencing
	// them in the command state.
	std::vector<DeviceHandle*> destroyed; // NOTE: use pool memory here, too?

	// Pipeline layouts we have to keep alive.
	CommandVector<IntrusivePtr<PipelineLayout>> pipeLayouts;

	bool finished {};
	std::atomic<u32> refCount {0};

	template<typename H>
	bool uses(const H& handle) const {
		if constexpr(std::is_same_v<H, Image>) {
			return images.find(handle.handle) != images.end();
		} else {
			return handles.find(handleToU64(fuen::handle(handle))) != handles.end();
		}
	}

	Device& device() const { return *memBlocks.get_deleter().dev; }

	CommandBufferRecord(CommandBuffer& cb);
	~CommandBufferRecord();
};

} // namespace fuen
