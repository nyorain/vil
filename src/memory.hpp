#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/dlg.hpp>
#include <set>
#include <variant>

namespace vil {

struct MemoryBind {
	DeviceMemory* memory {}; // this can be null, e.g. if the memory was destroyed
	MemoryResource* resource {};
	VkDeviceSize memOffset {};
	VkDeviceSize memSize {};

	struct CmpByMemOffset {
		bool operator()(const MemoryBind& a, const MemoryBind& b) const;
		bool operator()(const MemoryBind* a, const MemoryBind* b) const {
			return this->operator()(*a, *b);
		}
	};
};

// Describes the memory binding state of a non-sparse resource.
struct FullMemoryBind : MemoryBind {
	enum class State {
		unbound,
		bound,
		memoryDestroyed,
		resourceDestroyed,
	};

	State memState {State::unbound};
};

struct SparseMemoryBind : MemoryBind {
	VkSparseMemoryBindFlags flags {};
	bool opaque {};
};

struct OpaqueSparseMemoryBind : SparseMemoryBind {
	VkDeviceSize resourceOffset {};

	OpaqueSparseMemoryBind() {
		opaque = true;
	}

	struct Cmp {
		bool operator()(const OpaqueSparseMemoryBind& a,
			const OpaqueSparseMemoryBind& b) const;
	};
};

struct ImageSparseMemoryBind : SparseMemoryBind {
	VkImageSubresource subres {};
	VkOffset3D offset {};
	VkExtent3D size {};
	VkSparseMemoryBindFlags flags {};

	ImageSparseMemoryBind() {
		opaque = true;
	}

	struct Cmp {
		bool operator()(const ImageSparseMemoryBind& a,
			const ImageSparseMemoryBind& b) const;
	};
};

struct SparseMemoryState {
	std::set<OpaqueSparseMemoryBind,
		OpaqueSparseMemoryBind::Cmp> opaqueBinds;
	std::set<ImageSparseMemoryBind,
		ImageSparseMemoryBind::Cmp> imageBinds;
};

struct MemoryResource : SharedDeviceHandle {
	// TODO: can probably get rid of this
	VkObjectType memObjectType {};

	// Which member is active is only determined by creation flags
	// and immutable over the resources lifetime.
	std::variant<FullMemoryBind, SparseMemoryState> memory {};

	void onApiDestroy();
};

struct DeviceMemory : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_DEVICE_MEMORY;

	VkDeviceMemory handle {};

	u32 typeIndex {};
	VkDeviceSize size {};

	// Describes the current map state
	void* map {}; // nullptr if not mapped
	VkDeviceSize mapOffset {};
	VkDeviceSize mapSize {};

	// Sorted by memory offset. Keep in mind that allocations may alias. If
	// multiple allocations have the same offset, the sorting between those
	// is arbitrary.
	std::set<const MemoryBind*, MemoryBind::CmpByMemOffset> allocations;

	void onApiDestroy();
};

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
    VkDevice                                    device,
    const VkMemoryAllocateInfo*                 pAllocateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDeviceMemory*                             pMemory);

VKAPI_ATTR void VKAPI_CALL FreeMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize                                offset,
    VkDeviceSize                                size,
    VkMemoryMapFlags                            flags,
    void**                                      ppData);

VKAPI_ATTR void VKAPI_CALL UnmapMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory);

VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(
    VkDevice                                    device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges);

VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(
    VkDevice                                    device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges);

VKAPI_ATTR void VKAPI_CALL GetDeviceMemoryCommitment(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize*                               pCommittedMemoryInBytes);

} // namespace vil
