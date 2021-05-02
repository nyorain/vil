#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <util/intrusive.hpp>
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

	// VkDescriptorSetLayoutBinding, with extra data
	struct Binding {
		u32 binding;
		VkDescriptorType descriptorType;
		// You almost never want to use descriptorCount when dealing with a
		// descriptorSet, just use binding.size() to account for variable count bindings.
		u32 descriptorCount;
		VkShaderStageFlags stageFlags;
		std::unique_ptr<Sampler*[]> immutableSamplers;
		VkDescriptorBindingFlags flags; // for descriptor indexing
	};

	// Static after creation. Ordered by binding.
	std::vector<Binding> bindings;
	std::atomic<u32> refCount {0}; // intrusive ref count

	// Total number of bindings, including array elements
	std::size_t totalNumBindings {};

	// handle will be kept alive until this object is actually destroyed.
	~DescriptorSetLayout();
};

// State of a descriptor set. Disconnected from the DescriptorSet itself
// since for submission, we want to know the state of the descriptor at
// submission time even if the descriptor set itself has been changed or
// destroyed later on.
struct DescriptorSetState {
	struct ImageInfo {
		ImageView* imageView;
		Sampler* sampler; // even stored here if immutable in layout
		VkImageLayout layout;
	};

	struct BufferInfo {
		Buffer* buffer;
		VkDeviceSize offset;
		VkDeviceSize range;
	};

	struct Binding {
		bool valid {};

		union {
			ImageInfo imageInfo;
			BufferInfo bufferInfo;
			BufferView* bufferView;
		};
	};

	std::vector<std::vector<Binding>> bindings;
	u32 refCount {};
};

struct DescriptorSet : DeviceHandle {
	DescriptorPool* pool {};
	IntrusivePtr<DescriptorSetLayout> layout {};
	VkDescriptorSet handle {};

	IntrusivePtr<DescriptorSetState> state;

	struct ImageInfo {
		ImageView* imageView;
		Sampler* sampler; // even stored here if immutable in layout
		VkImageLayout layout;
	};

	struct BufferInfo {
		Buffer* buffer;
		VkDeviceSize offset;
		VkDeviceSize range;
	};

	struct Binding {
		bool valid {};

		union {
			ImageInfo imageInfo;
			BufferInfo bufferInfo;
			BufferView* bufferView;
		};
	};

	std::vector<std::vector<Binding>> bindings;

	Sampler* getSampler(unsigned binding, unsigned elem);
	ImageView* getImageView(unsigned binding, unsigned elem);
	Buffer* getBuffer(unsigned binding, unsigned elem);
	BufferView* getBufferView(unsigned binding, unsigned elem);

	~DescriptorSet();
};

// Notifies the given descriptor that 'handle' - bound in the
// descriptor at binding, elem - was destroyed.
// Must only be called while device mutex is locked.
// Will invalidate the command records connected to the ds.
void notifyDestroyLocked(DescriptorSet& ds, unsigned binding, unsigned elem,
		const Handle& handle);

struct DescriptorUpdateTemplate : DeviceHandle {
	VkDescriptorUpdateTemplate handle {};

	// Intrusive ref count. Needed e.g. for command buffer recording
	std::atomic<u32> refCount {0};

	std::vector<VkDescriptorUpdateTemplateEntry> entries;
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
