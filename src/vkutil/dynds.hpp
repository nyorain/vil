#pragma once

#include <fwd.hpp>
#include <vkutil/handles.hpp>
#include <threadContext.hpp>
#include <memory>
#include <deque>

namespace vil::vku {

struct BufferSpan;

// Tracked DescriptorSetLayout, knows all its bindings.
// Mainly useful when one wants to simply create a descriptor set from
// a layout, see DynDs.
class DynDsLayout : public DescriptorSetLayout {
public:
	DynDsLayout() = default;
	DynDsLayout(Device& dev, const VkDescriptorSetLayoutCreateInfo&, StringParam name);
	DynDsLayout(Device& dev, span<VkDescriptorSetLayoutBinding>,
		StringParam name, span<const VkDescriptorBindingFlags> = {});
	~DynDsLayout() = default;

	// Moving a DynDsLayout would be a problem for all DynDs's that
	// reference it.
	DynDsLayout(DynDsLayout&&) = delete;
	DynDsLayout& operator=(DynDsLayout&&) = delete;

	// Calling this on an already initialized TrDsLayout works,
	// but will trigger UB when there are DynDs objects referencing it.
	void init(Device& dev, const VkDescriptorSetLayoutCreateInfo&,
		StringParam name);
	void init(Device& dev, span<VkDescriptorSetLayoutBinding>,
		StringParam name, span<const VkDescriptorBindingFlags> = {});

	const auto& bindings() const { return bindings_; }
	const auto& flags() const { return flags_; } // might be empty

protected:
	void doInit(span<const VkDescriptorSetLayoutBinding>);

	// must be ordered but may contain holes.
	std::vector<VkDescriptorSetLayoutBinding> bindings_;
	std::vector<VkDescriptorBindingFlags> flags_;
};

// Tracked DescriptorSet, knows its associated pool and layout.
// created with.
// Must be created from a pool with the freeDescriptorSet flag set.
class DynDs : public Resource<DynDs, VkDescriptorSet, VK_OBJECT_TYPE_DESCRIPTOR_SET> {
public:
	DynDs() = default;
	DynDs(VkDescriptorPool, const DynDsLayout&, VkDescriptorSet);
	~DynDs() { destroy(); }

	DynDs(DynDs&& rhs) noexcept = default;
	DynDs& operator=(DynDs&& rhs) noexcept = default;

	void destroy();

	VkDescriptorPool pool() const { return pool_; }
	const DynDsLayout& layout() const { return *layout_; }

protected:
	VkDescriptorPool pool_ {};
	const DynDsLayout* layout_ {};
};

class DescriptorUpdate {
public:
	// Will automatically apply the update in the destructor.
	explicit DescriptorUpdate(DynDs& ds);

	// If constructed with this, must manually call apply, passing
	// the Device and DescriptorSet.
	// If neither apply not reset is called until this object is destroyed,
	// will output a warning.
	DescriptorUpdate(
		span<const VkDescriptorSetLayoutBinding> bindings,
		span<const VkDescriptorBindingFlags> flags = {});

	~DescriptorUpdate();

	// copying a descriptor update doesn't really make sense
	DescriptorUpdate(const DescriptorUpdate&) = delete;
	DescriptorUpdate& operator=(const DescriptorUpdate&) = delete;

	DescriptorUpdate(DescriptorUpdate&&) noexcept = delete;
	DescriptorUpdate& operator=(DescriptorUpdate&&) noexcept = delete;

	void apply(Device& dev, VkDescriptorSet);

	void set(BufferSpan span);
	// vk::ImageLayout::undefined will results in the layout being automatically
	// chosen for the associated descriptor type:
	// - shaderReadOnlyOptimal for all sampled & inputAttachment descriptors
	// - general for storageImage descriptors
	void set(VkImageView view,
		VkImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		VkSampler = {});
	void set(VkImageView view, VkSampler);
	void set(VkSampler);
	void set(VkAccelerationStructureKHR);
	void set(span<const BufferSpan>);
	void set(const VkWriteDescriptorSet& write);

	template<typename... Args>
	DescriptorUpdate& operator()(const Args&... args) {
		set(args...);
		return *this;
	}

	void seek(unsigned binding);
	void skip(unsigned inc = 1u);
	void reset();

private:
	bool checkSkip();

private:
	ThreadMemScope memScope_;
	DynDs* ds_ {};

	unsigned currentBinding_ = 0; // binding to update next
	unsigned currentID_ = 0; // position of that binding in bindings_

	using Alloc = LinearScopedAllocator<VkWriteDescriptorSet>;
	std::vector<VkWriteDescriptorSet, Alloc> writes_;

	// must be ordered but may contain holes.
	span<const VkDescriptorSetLayoutBinding> bindings_;
	span<const VkDescriptorBindingFlags> flags_;
};

} // namespace vil::vku

