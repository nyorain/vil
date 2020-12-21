#pragma once

#include <device.hpp>
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
		std::weak_ptr<ImageView> imageView;
		std::weak_ptr<Sampler> sampler;
		VkImageLayout layout;
	};

	struct BufferInfo {
		std::weak_ptr<Buffer> buffer;
		VkDeviceSize offset;
		VkDeviceSize range;
	};

	struct Binding {
		bool valid {};

		// note: strictly speaking we wouldn't need the type tag here,
		// we could deduce it from the layout. But we need weak_ptr's
		// and using a union with non-trivial types is a pain in the ass.
		std::variant<ImageInfo, BufferInfo, std::weak_ptr<BufferView>> data;
	};

	std::vector<std::vector<Binding>> bindings;

	// std::weak_ptr<Sampler> getSampler(unsigned binding, unsigned elem);
	// std::weak_ptr<ImageView> getImageView(unsigned binding, unsigned elem);
	// std::weak_ptr<Buffer> getBuffer(unsigned binding, unsigned elem);
	// std::weak_ptr<BufferView> getBufferView(unsigned binding, unsigned elem);

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
