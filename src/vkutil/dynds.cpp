#include <vkutil/dynds.hpp>
#include <vkutil/bufferSpan.hpp>
#include <util/linalloc.hpp>
#include <device.hpp>

namespace vil::vku {

// DynDsLayout
DynDsLayout::DynDsLayout(Device& dev, const VkDescriptorSetLayoutCreateInfo& info,
		StringParam name) {
	this->init(dev, info, name);
}

DynDsLayout::DynDsLayout(Device& dev, span<VkDescriptorSetLayoutBinding> bindings,
			StringParam name, span<const VkDescriptorBindingFlags> flags) {
	this->init(dev, bindings, name, flags);
}

void DynDsLayout::init(Device& dev, const VkDescriptorSetLayoutCreateInfo& info,
		StringParam name) {
	DescriptorSetLayout::operator=(DescriptorSetLayout{dev, info, name});

	if(info.pNext) {
		auto flags = static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(info.pNext);
		if(flags->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
			flags_ = {flags->pBindingFlags, flags->pBindingFlags + flags->bindingCount};
		} else {
			dlg_error("Unsupported extension used");
		}
	}

	doInit({info.pBindings, info.bindingCount});
}

void DynDsLayout::init(Device& dev, span<VkDescriptorSetLayoutBinding> bindings,
		StringParam name, span<const VkDescriptorBindingFlags> flags) {
	DescriptorSetLayout::operator=(DescriptorSetLayout{dev, bindings, name, flags});
	flags_ = {flags.begin(), flags.end()};
	doInit(bindings);
}

void DynDsLayout::doInit(span<const VkDescriptorSetLayoutBinding> bindings) {
	bindings_ = {bindings.begin(), bindings.end()};
}

// DynDs
DynDs::DynDs(VkDescriptorPool pool, const DynDsLayout& layout, VkDescriptorSet ds) {
	dlg_assert(ds);
	dlg_assert(pool);
	dlg_assert(layout.vkHandle());

	dev_ = &layout.dev();
	handle_ = ds;
	pool_ = pool;
	layout_ = &layout;
}

void DynDs::destroy() {
	if(handle_) {
		dlg_assert(layout_ && layout_->vkHandle());
		dlg_assert(pool_);
		dev_->dispatch.FreeDescriptorSets(dev_->handle, pool_, 1, &handle_);
		handle_ = {};
		dev_ = {};
		layout_ = {};
		pool_ = {};
	}
}

// DescriptorUpdate
DescriptorUpdate::DescriptorUpdate(DynDs& ds) :
	ds_(&ds), writes_(memScope_), bindings_(ds.layout().bindings()), flags_(ds.layout().flags()) {
}

DescriptorUpdate::DescriptorUpdate(
	span<const VkDescriptorSetLayoutBinding> bindings,
	span<const VkDescriptorBindingFlags> flags) :
		writes_(memScope_), bindings_(bindings), flags_(flags) {
}

DescriptorUpdate::~DescriptorUpdate() {
	if(!writes_.empty()) {
		if(ds_) {
			apply(ds_->dev(), ds_->vkHandle());
		} else {
			dlg_warn("~DescriptorUpdate: missing target ds");
		}
	}
}

bool DescriptorUpdate::checkSkip() {
	if(currentID_ >= bindings_.size()) {
		dlg_debug("Binding {} too high, ignoring update", currentBinding_);
		++currentBinding_;
		return true;
	}

	dlg_assert(bindings_[currentID_].binding >= currentBinding_);
	if(bindings_[currentID_].binding > currentBinding_) {
		dlg_debug("Ds has no binding for {}, ignoring update", currentBinding_);
		++currentBinding_;
		return true;
	}

	if(bindings_[currentID_].descriptorCount == 0) {
		dlg_debug("No descriptors for binding {}, ignoring update", currentBinding_);
		++currentBinding_;
		return true;
	}

	return false;
}

void DescriptorUpdate::set(BufferSpan bspan) {
	set(span<const BufferSpan>{{bspan}});
}

void DescriptorUpdate::set(span<const BufferSpan> spans) {
	if(checkSkip()) {
		return;
	}

	auto type = bindings_[currentID_].descriptorType;
	dlg_assert(
		type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
		type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
		type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
		type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
	dlg_assert(bindings_[currentID_].descriptorCount == spans.size());

	auto infos = memScope_.alloc<VkDescriptorBufferInfo>(spans.size());
	for(auto i = 0u; i < spans.size(); ++i) {
		infos[i].buffer = spans[i].buffer;
		infos[i].offset = spans[i].offset();
		infos[i].range = spans[i].size();
	}

	auto& write = writes_.emplace_back();
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = spans.size();
	write.dstBinding = currentBinding_;
	write.pBufferInfo = infos.data();
	write.descriptorType = type;
	write.dstSet = ds_ ? ds_->vkHandle() : VK_NULL_HANDLE;

	++currentBinding_;
	++currentID_;
}

void DescriptorUpdate::set(VkImageView view, VkImageLayout layout, VkSampler sampler) {
	if(checkSkip()) {
		return;
	}

	auto type = bindings_[currentID_].descriptorType;

	if(type == VK_DESCRIPTOR_TYPE_SAMPLER) {
		dlg_assert(sampler);
		dlg_assert(!view);
		dlg_assert(layout == VK_IMAGE_LAYOUT_UNDEFINED);
	} else if(type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
		dlg_assert(sampler || bindings_[currentID_].pImmutableSamplers);
		if(layout == VK_IMAGE_LAYOUT_UNDEFINED) {
			layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
	} else if(type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
		dlg_assert(!sampler);
		if(layout == VK_IMAGE_LAYOUT_UNDEFINED) {
			layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
	} else if(type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
		dlg_assert(!sampler);
		if(layout == VK_IMAGE_LAYOUT_UNDEFINED) {
			layout = VK_IMAGE_LAYOUT_GENERAL;
		}
	}

	auto info = memScope_.alloc<VkDescriptorImageInfo>(1);
	info[0].imageLayout = layout;
	info[0].sampler = sampler;
	info[0].imageView = view;

	auto& write = writes_.emplace_back();
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1u;
	write.dstBinding = currentBinding_;
	write.pImageInfo = info.data();
	write.descriptorType = type;
	write.dstSet = ds_ ? ds_->vkHandle() : VK_NULL_HANDLE;

	++currentBinding_;
	++currentID_;
}

void DescriptorUpdate::set(VkImageView view, VkSampler sampler) {
	set(view, VK_IMAGE_LAYOUT_UNDEFINED, sampler);
}
void DescriptorUpdate::set(VkSampler sampler) {
	set(VkImageView {}, VK_IMAGE_LAYOUT_UNDEFINED, sampler);
}

void DescriptorUpdate::set(VkAccelerationStructureKHR accelStruct) {
	if(checkSkip()) {
		return;
	}

	auto type = bindings_[currentID_].descriptorType;
	dlg_assert(type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);

	auto& ASRef = memScope_.construct<VkAccelerationStructureKHR>(accelStruct);
	auto& dstAS = memScope_.construct<VkWriteDescriptorSetAccelerationStructureKHR>();
	dstAS.accelerationStructureCount = 1u;
	dstAS.pAccelerationStructures = &ASRef;

	auto& write = writes_.emplace_back();
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1u;
	write.dstBinding = currentBinding_;
	write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	write.pNext = &dstAS;
	write.dstSet = ds_ ? ds_->vkHandle() : VK_NULL_HANDLE;

	++currentBinding_;
	++currentID_;
}

void DescriptorUpdate::set(const VkWriteDescriptorSet& write) {
	dlg_assert(write.dstBinding == currentBinding_);
	dlg_assert(bindings_.size() > currentID_);
	dlg_assert(bindings_[currentID_].binding == currentBinding_);
	writes_.push_back(write);

	++currentBinding_;
	++currentID_;
}

void DescriptorUpdate::seek(unsigned binding) {
	if(binding < currentBinding_) {
		currentID_ = 0u;
	}

	while(currentID_ < bindings_.size() && bindings_[currentID_].binding < binding) {
		++currentID_;
	}

	currentBinding_ = binding;
}
void DescriptorUpdate::skip(unsigned inc) {
	currentBinding_ += inc;
	while(currentID_ < bindings_.size() && bindings_[currentID_].binding < currentBinding_) {
		++currentID_;
	}
}
void DescriptorUpdate::apply(Device& dev, VkDescriptorSet ds) {
	for(auto& write : writes_) {
		write.dstSet = ds;
	}

	dev.dispatch.UpdateDescriptorSets(dev.handle, writes_.size(), writes_.data(), 0u, nullptr);
	reset();
}

void DescriptorUpdate::reset() {
	writes_ = {};
	currentBinding_ = 0u;
	currentID_ = 0u;
}

} // namespace vil::vku
