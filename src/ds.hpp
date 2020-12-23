#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <intrusive.hpp>
#include <vulkan/vulkan.h>

#include <optional>
#include <variant>

namespace fuen {

// Which member in descriptor binding is valid
enum class DescriptorCategory {
	none,
	image,
	buffer,
	bufferView
};

DescriptorCategory category(VkDescriptorType);
bool needsSampler(VkDescriptorType);
bool needsImageView(VkDescriptorType);

struct DescriptorPool : DeviceHandle {
	VkDescriptorPool handle {};
	std::vector<DescriptorSet*> descriptorSets;

	u32 maxSets {};
	std::vector<VkDescriptorPoolSize> poolSizes {};

	~DescriptorPool();
};

struct DescriptorSetLayout : DeviceHandle {
	VkDescriptorSetLayout handle {};

	// Static after creation. Ordered by binding.
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	std::vector<std::vector<VkSampler>> immutableSamplers;

	std::atomic<u32> refCount {0}; // intrusive ref count

	// handle will be kept alive until this object is actually destroyed.
	~DescriptorSetLayout();
};

struct DescriptorSet : DeviceHandle {
	DescriptorPool* pool {};
	IntrusivePtr<DescriptorSetLayout> layout {};
	VkDescriptorSet handle {};

	// TODO: support buffer views
	struct ImageInfo {
		ImageView* imageView;
		Sampler* sampler;
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

		// note: strictly speaking we wouldn't need the type tag here,
		// we could deduce it from the layout. But we need(ed) weak_ptr's
		// and using a union with non-trivial types is a pain in the ass.
		// std::variant<ImageInfo, BufferInfo, BufferView*> data;
	};

	std::vector<std::vector<Binding>> bindings;

	Sampler* getSampler(unsigned binding, unsigned elem);
	ImageView* getImageView(unsigned binding, unsigned elem);
	Buffer* getBuffer(unsigned binding, unsigned elem);
	BufferView* getBufferView(unsigned binding, unsigned elem);

	~DescriptorSet();
};

void unregisterLocked(DescriptorSet& ds, unsigned binding, unsigned elem);

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
struct DescriptorUpdateTemplate : DeviceHandle {
	VkDescriptorUpdateTemplate handle {};

	std::vector<VkDescriptorUpdateTemplateEntry> entries;
};

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

} // namespace fuen
