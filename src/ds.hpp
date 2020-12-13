#pragma once

#include "device.hpp"
#include <optional>

namespace fuen {

// Which member in descriptor binding is valid
enum class DescriptorCategory {
	none,
	image,
	buffer,
	bufferView
};

DescriptorCategory category(VkDescriptorType);

struct DescriptorPool : DeviceHandle {
	VkDescriptorPool handle {};
	std::vector<DescriptorSet*> descriptorSets;

	~DescriptorPool();
};

struct DescriptorSetLayout : DeviceHandle {
	VkDescriptorSetLayout handle {};

	// Static after creation. Ordered by binding.
	std::vector<VkDescriptorSetLayoutBinding> bindings;

	// handle will be kept alive until this object is actually destroyed.
	~DescriptorSetLayout();
};

struct DescriptorSet : DeviceHandle {
	DescriptorPool* pool {};
	std::shared_ptr<DescriptorSetLayout> layout {};
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
	};

	std::vector<std::vector<Binding>> bindings;

	// Invalidates the given binding & element, resetting them.
	// Will also invalidate command buffers, if needed.
	// Expects the device mutex to be locked.
	void invalidateLocked(unsigned binding, unsigned elem);

	Sampler* getSampler(unsigned binding, unsigned elem);
	ImageView* getImageView(unsigned binding, unsigned elem);
	Buffer* getBuffer(unsigned binding, unsigned elem);
	BufferView* getBufferView(unsigned binding, unsigned elem);

	~DescriptorSet();
};

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

} // namespace fuen
