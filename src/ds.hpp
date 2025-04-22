#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
#include <util/debugMutex.hpp>
#include <util/profiling.hpp>
#include <nytl/span.hpp>
#include <vk/vulkan.h>

#include <memory>
#include <atomic>

namespace vil {

struct DescriptorStateCopy;

// Describes the type of the descriptor data.
enum class DescriptorCategory {
	none,
	image,
	buffer,
	bufferView,
	inlineUniformBlock,
	accelStruct,
};

DescriptorCategory category(VkDescriptorType);
// Returns just whether the given descriptor type requires a sampler in general.
// The sampler might still be bound immutably in ds layout.
bool needsSampler(VkDescriptorType);
// This returns false if the DescriptorSetLayout has the sampler
// already statically bound as immutable sampler.
bool needsBoundSampler(const DescriptorSetLayout&, unsigned binding);
bool needsImageView(VkDescriptorType);
bool needsImageLayout(VkDescriptorType);
bool needsDynamicOffset(VkDescriptorType);

// Lifetime of these objects bound to lifetime of their DescriptorPool.
// Pool mutex is locked when offset/size/set members are modified.
struct DescriptorPoolSetEntry {
	// NOTE: could compute offset, size from the referenced set.
	// But it's not that expensive to store them here and might be faster.
	u32 offset {};
	u32 size {};
	DescriptorPoolSetEntry* next {};
	DescriptorPoolSetEntry* prev {};
	DescriptorSet* set {};
};

struct DescriptorPool : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_DESCRIPTOR_POOL;

	VkDescriptorPool handle {};
	VkDescriptorPoolCreateFlags flags {};

	u32 maxSets {};
	u32 lastID {};
	std::vector<VkDescriptorPoolSize> poolSizes {};

	// The mutex used to access the entries.
	// While this mutex is locked, no sets from the pool will be
	// created or destroyed.
	TracyLockable(DebugMutex, mutex);

	using SetEntry = DescriptorPoolSetEntry;

	// Descriptor data: We just allocate one large buffer on
	// DescriptorPool creation and then suballocate that to the individual
	// descriptor sets. We want to guarantee fast allocation and
	// freeing (even when using FreeDescriptorSets instead of
	// ResetDescriptorPool) we use a linked list of entries.
	u32 dataSize {};
	std::unique_ptr<std::byte[]> data;
	std::unique_ptr<SetEntry[]> entries;

	// Linked list of the alive descriptor sets, sorted by offset.
	SetEntry* usedEntries {};

	// The last link of the usedEntries list.
	SetEntry* highestEntry {};

	// The entry we last allocated that wasn't highestEntry.
	// Only used for fragmentating pools.
	SetEntry* lastEntry {};

	// Linked list of unused SetEntry objects. NOT a list of free spaces.
	SetEntry* freeEntries {};

	~DescriptorPool();
};

struct DescriptorSetLayout : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;

	VkDescriptorSetLayout handle {};
	VkDescriptorSetLayoutCreateFlags flags {};

	// VkDescriptorSetLayoutBinding, with extra data
	struct Binding {
		u32 offset {}; // total offset in bytes into binding data of DescriptorSetState
		// You almost never want to use descriptorCount when dealing with a
		// descriptorSet, just use binding.size() to account for variable count bindings.
		u32 descriptorCount {};
		VkDescriptorType descriptorType {VK_DESCRIPTOR_TYPE_MAX_ENUM};
		VkShaderStageFlags stageFlags {};
		std::unique_ptr<IntrusivePtr<Sampler>[]> immutableSamplers;
		VkDescriptorBindingFlags flags {}; // for descriptor indexing
		u32 dynOffset {u32(-1)}; // offset into dynamic offset array
	};

	// Immutable after creation. Can be empty per vulkan spec.
	// Allows fast random access via bindingID.
	std::vector<Binding> bindings;

	// The total number of dynamic buffer descriptors.
	// Needed for quick binding in CmdBindDescriptorSets.
	// We can know this statically since dynamic buffers can't have the
	// variable_descriptor_count flag
	// (VUID-VkDescriptorSetLayoutBindingFlagsCreateInfo-pBindingFlags-03015)
	u32 numDynamicBuffers {};

	// Whether any bindings have immutableSamplers. We remember this
	// as an optimization, so we don't iterate over bindings on
	// AllocateDescriptorSets just to find there are not immutable samplers
	// to initialize.
	bool immutableSamplers {};

	// handle will be kept alive until this object is actually destroyed.
	~DescriptorSetLayout();
};

// Returns whether the two given DescriptorSetLayouts are compatible
// in the sense of the Vulkan specification
// NOTE: does only compare sampler identity, no deep-equality
bool compatible(const DescriptorSetLayout&, const DescriptorSetLayout& b,
		bool checkImmutableSamplers = true);

// Returns whether the two given DescriptorSetLayouts have bindings that
// conflict with each other. Each layout can have bindings that the other
// set does not have, as long as their common bindings have the same
// type (and immutable samplers if 'checkImmutableSamplers' is true).
// NOTE: does only compare sampler identity, no deep-equality
bool conflicting(const DescriptorSetLayout&, const DescriptorSetLayout& b,
		bool checkImmutableSamplers = false);

// Information about a single binding in a DescriptorSet.
struct ImageDescriptor {
	ImageView* imageView {};
	Sampler* sampler {}; // even stored here if immutable in layout
	VkImageLayout layout {};
};

struct BufferDescriptor {
	Buffer* buffer {};
	VkDeviceSize offset {};
	VkDeviceSize range {};
};

struct BufferViewDescriptor {
	BufferView* bufferView {};
};

struct AccelStructDescriptor {
	AccelStruct* accelStruct {};
};

inline bool operator==(const ImageDescriptor& a, const ImageDescriptor& b) {
	return a.imageView == b.imageView &&
		a.sampler == b.sampler &&
		a.layout == b.layout;
}

inline bool operator==(const BufferDescriptor& a, const BufferDescriptor& b) {
	return a.buffer == b.buffer &&
		a.offset == b.offset &&
		a.range == b.range;
}

inline bool operator==(const BufferViewDescriptor& a, const BufferViewDescriptor& b) {
	return a.bufferView == b.bufferView;
}

inline bool operator==(const AccelStructDescriptor& a, const AccelStructDescriptor& b) {
	return a.accelStruct == b.accelStruct;
}

// Temporary reference to descriptor state.
struct DescriptorStateRef {
	DescriptorSetLayout* layout {};
	std::byte* data {};
	u32 variableDescriptorCount {};

	DescriptorStateRef() = default;
	DescriptorStateRef(const DescriptorSet&);

	// explicit since usually accessed via 'access(DescriptorSetCow)'
	explicit DescriptorStateRef(DescriptorStateCopy&);
};

// NOTE: keep in mind that descriptorCount is allowed to be 0
u32 descriptorCount(DescriptorStateRef, unsigned binding);
u32 totalDescriptorCount(DescriptorStateRef);

// NOTE: retrieving the span itself does not need to lock the state's
// mutex. The caller must manually synchronize access to the bindings by locking
// the state's mutex.
span<BufferDescriptor> buffers(DescriptorStateRef, unsigned binding);
span<ImageDescriptor> images(DescriptorStateRef, unsigned binding);
span<BufferViewDescriptor> bufferViews(DescriptorStateRef, unsigned binding);
span<AccelStructDescriptor> accelStructs(DescriptorStateRef, unsigned binding);
span<std::byte> inlineUniformBlock(DescriptorStateRef, unsigned binding);

// Returns whether the given descriptor state has the given handle bound.
// For Buffers and Images, also returns true when one of their bufferViews/
// imageViews is bound.
bool hasBound(DescriptorStateRef, const Handle& handle);

struct DescriptorStateCopy {
	struct Deleter {
		void operator()(DescriptorStateCopy* ptr) const;
	};

	IntrusivePtr<DescriptorSetLayout> layout {};
	u32 variableDescriptorCount {};
	u32 _pad {};

	// std::byte data[]; // following this in memory
};

using DescriptorStateCopyPtr = std::unique_ptr<DescriptorStateCopy, DescriptorStateCopy::Deleter>;

// Vulkan descriptor set handle.
// PERF: make sure to keep this as small as possible. Space for it is
//   statically allocated on descriptorPool creation.
// NOTE: other than most handles, DescriptorSet doesn't have shared
//   ownership. The reason for this is mainly that we can very efficiently
//   allocate them via the DescriptorPool (wouldn't be possible if all
//   of them could be kept alive).
struct DescriptorSet : Handle {
public:
	static constexpr auto objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET;

	// Immutable after creation.
	DescriptorPool* pool {};
	VkDescriptorSet handle {};

	IntrusivePtr<DescriptorSetLayout> layout {};
	DescriptorPoolSetEntry* setEntry {};
	u32 id {};
	u32 variableDescriptorCount {};

public:
	Device& dev() const { return *pool->dev; }

	// requires device *and* pool mutex to be locked
	IntrusivePtr<DescriptorSetCow> addCowLocked();
	std::unique_lock<LockableBase(DebugMutex)> checkResolveCow();
	std::unique_lock<LockableBase(DebugMutex)> lock() {
		dlg_assert(pool);
		return std::unique_lock<LockableBase(DebugMutex)>(pool->mutex);
	}

	// requires device *and* pool mutex to be locked
	DescriptorStateCopyPtr validateAndCopyLocked();

private:
	DescriptorStateCopyPtr copyLockedState();

private:
	// Protected by pool->mutex
	// Not owned here. The destructor of DescriptorSetCow automatically
	// unsets this.
	IntrusivePtr<DescriptorSetCow> cow_ {};

	// Following this in memory
	// Protected by pool->mutex
	// std::byte bindingData[];
};

// Copy-on-write mechanism on a descriptor state.
// See DescriptorSet::cow.
struct DescriptorSetCow {
	// Mutex protects ds and copy. Needed since accessing the cow
	// and resolving it may happen in parallel from multiple threads.
	DebugMutex mutex;

	// Only set when the cow still references the descriptor sets original
	// content. Otherwise null. Once unset, won't be set again.
	DescriptorSet* ds {};

	// Only set when the cow has made its own copy. Otherwise null.
	// Once set, won't be unset again.
	DescriptorStateCopyPtr copy {};

	// DescriptorSetCow is intrusively reference counted since multiple
	// consumers may want to reference the same descriptor state.
	std::atomic<u32> refCount {};

	DescriptorSetCow() = default;
	~DescriptorSetCow();
};

std::pair<DescriptorStateRef, std::unique_lock<DebugMutex>> access(DescriptorSetCow& cow);

struct DescriptorUpdateTemplate : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE;

	VkDescriptorUpdateTemplate handle {};
	std::vector<VkDescriptorUpdateTemplateEntry> entries;

	~DescriptorUpdateTemplate();
};

// calculates the total size in bytes the data of a descriptor set update
// with the given template must have.
u32 totalUpdateDataSize(const DescriptorUpdateTemplate&);

// API
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(
    VkDevice                                    device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorSetLayout*                      pSetLayout);

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(
    VkDevice                                    device,
    VkDescriptorSetLayout                       descriptorSetLayout,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(
    VkDevice                                    device,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorPool*                           pDescriptorPool);

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    VkDescriptorPoolResetFlags                  flags);

VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(
    VkDevice                                    device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets);

VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    descriptorSetCount,
    const VkDescriptorSet*                      pDescriptorSets);

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(
    VkDevice                                    device,
    uint32_t                                    descriptorWriteCount,
    const VkWriteDescriptorSet*                 pDescriptorWrites,
    uint32_t                                    descriptorCopyCount,
    const VkCopyDescriptorSet*                  pDescriptorCopies);

// vulkan 1.1: descriptor update templates
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(
    VkDevice                                    device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorUpdateTemplate*                 pDescriptorUpdateTemplate);

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(
    VkDevice                                    device,
    VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(
    VkDevice                                    device,
    VkDescriptorSet                             descriptorSet,
    VkDescriptorUpdateTemplate                  descriptorUpdateTemplate,
    const void*                                 pData);

// vulkan 1.1
VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutSupport(
    VkDevice                                    device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    VkDescriptorSetLayoutSupport*               pSupport);

} // namespace vil
