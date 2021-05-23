#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
#include <util/span.hpp>
#include <util/debugMutex.hpp>
#include <util/profiling.hpp>
#include <vk/vulkan.h>

#include <optional>
#include <variant>
#include <memory>
#include <atomic>

namespace vil {

// Which member in descriptor binding is valid
enum class DescriptorCategory {
	none,
	image,
	buffer,
	bufferView
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

struct DescriptorPool : DeviceHandle {
	VkDescriptorPool handle {};
	std::vector<DescriptorSet*> descriptorSets;

	u32 maxSets {};
	std::vector<VkDescriptorPoolSize> poolSizes {};

	~DescriptorPool();
};

struct DescriptorSetLayout : DeviceHandle {
	VkDescriptorSetLayout handle {};
	VkDescriptorSetLayoutCreateFlags flags {};

	// VkDescriptorSetLayoutBinding, with extra data
	struct Binding {
		u32 binding;
		u32 offset; // total offset in bindings array of DescriptorSetState
		VkDescriptorType descriptorType;
		// You almost never want to use descriptorCount when dealing with a
		// descriptorSet, just use binding.size() to account for variable count bindings.
		u32 descriptorCount;
		VkShaderStageFlags stageFlags;
		std::unique_ptr<IntrusivePtr<Sampler>[]> immutableSamplers;
		VkDescriptorBindingFlags flags; // for descriptor indexing
	};

	// Static after creation. Ordered by binding. Can be empty per vulkan spec.
	std::vector<Binding> bindings;
	std::atomic<u32> refCount {0}; // intrusive ref count

	// handle will be kept alive until this object is actually destroyed.
	~DescriptorSetLayout();
};

size_t totalNumBindings(const DescriptorSetLayout&, u32 variableDescriptorCount);
bool compatible(const DescriptorSetLayout&, const DescriptorSetLayout& b);

// Information about a single binding in a DescriptorSet.
struct ImageDescriptor {
	IntrusivePtr<ImageView> imageView;
	IntrusivePtr<Sampler> sampler; // even stored here if immutable in layout
	VkImageLayout layout {};
};

inline bool operator==(const ImageDescriptor& a, const ImageDescriptor& b) {
	return a.imageView == b.imageView &&
		a.sampler == b.sampler &&
		a.layout == b.layout;
}

struct BufferDescriptor {
	IntrusivePtr<Buffer> buffer;
	VkDeviceSize offset {};
	VkDeviceSize range {};
};

inline bool operator==(const BufferDescriptor& a, const BufferDescriptor& b) {
	return a.buffer == b.buffer &&
		a.offset == b.offset &&
		a.range == b.range;
}

using BufferViewDescriptor = IntrusivePtr<BufferView>;

// State of a descriptor set. Disconnected from the DescriptorSet itself
// since for submission, we want to know the state of the descriptor at
// submission time even if the descriptor set itself has been changed or
// destroyed later on.
// The memory for the DescriptorBindings will be allocated together with
// the DescriptorSetState object, directly after it in memory.
// See the 'binding' functions below to access it.
struct DescriptorSetState {
	struct PtrHandler {
		void inc(DescriptorSetState&) const noexcept;
		void dec(DescriptorSetState&) const noexcept;
	};

	// The ds with which this state is associated.
	// If the ds the state originated from was changed or destroyed,
	// this will be null.
	DescriptorSet* ds {};

	// The layout associated with this state. Always valid.
	IntrusivePtr<DescriptorSetLayout> layout {};

	// If the state has a variable_descriptor_count binding, this
	// holds its count.
	u32 variableDescriptorCount {};

	std::atomic<u32> refCount {}; // intrusive reference count

	vilDefMutex(mutex);
};

using DescriptorSetStatePtr = HandledPtr<DescriptorSetState, DescriptorSetState::PtrHandler>;

u32 descriptorCount(const DescriptorSetState&, unsigned binding);

// NOTE: while retrieving the span itself does not need to lock the state's
// mutex. The caller must manually synchronize access to the bindings by locking
// the state's mutex.
span<BufferDescriptor> buffers(DescriptorSetState&, unsigned binding);
span<const BufferDescriptor> buffers(const DescriptorSetState&, unsigned binding);
span<ImageDescriptor> images(DescriptorSetState&, unsigned binding);
span<const ImageDescriptor> images(const DescriptorSetState&, unsigned binding);
span<BufferViewDescriptor> bufferViews(DescriptorSetState&, unsigned binding);
span<const BufferViewDescriptor> bufferViews(const DescriptorSetState&, unsigned binding);

// Vulkan descriptor set handle
struct DescriptorSet : DeviceHandle {
	DescriptorPool* pool {};
	VkDescriptorSet handle {};

	// synchronized via dev.mutex, i.e. pointer must only be changed/copied
	// while dev.mutex is locked.
	DescriptorSetStatePtr state;

	~DescriptorSet();
};

// Notifies the given descriptor state that 'handle' - bound in the
// descriptor at binding, elem - was destroyed.
// Must only be called while device mutex is locked.
// Will invalidate the command records connected to the ds.
void notifyDestroyLocked(DescriptorSetState& ds, unsigned binding, unsigned elem,
		const Handle& handle);

struct DescriptorUpdateTemplate : DeviceHandle {
	VkDescriptorUpdateTemplate handle {};

	// Intrusive ref count. Needed e.g. for command buffer recording
	std::atomic<u32> refCount {0};

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

} // namespace vil
