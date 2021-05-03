#include <buffer.hpp>
#include <device.hpp>
#include <data.hpp>
#include <ds.hpp>

namespace vil {

VkDeviceSize evalRange(VkDeviceSize fullSize, VkDeviceSize offset, VkDeviceSize range) {
	dlg_assert(offset <= fullSize);
	return range == VK_WHOLE_SIZE ? fullSize - offset : range;
}

// Classes
Buffer::~Buffer() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);
	for(auto* view : this->views) {
		view->buffer = nullptr;
	}

	// Can't use for loop here, as descriptor will unregsiter themselves in turn
	while(!this->descriptors.empty()) {
		auto& dsRef = *this->descriptors.begin();
		dlg_assert(getBuffer(*dsRef.state, dsRef.binding, dsRef.elem) == this);
		notifyDestroyLocked(*dsRef.state, dsRef.binding, dsRef.elem, *this);
	}
}

BufferView::~BufferView() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	if(this->buffer) {
		auto it = std::find(this->buffer->views.begin(), this->buffer->views.end(), this);
		dlg_assert(it != this->buffer->views.end());
		this->buffer->views.erase(it);
	}

	// Can't use for loop here, as descriptor will unregsiter themselves in turn
	while(!this->descriptors.empty()) {
		auto& dsRef = *this->descriptors.begin();
		dlg_assert(getBufferView(*dsRef.state, dsRef.binding, dsRef.elem) == this);
		notifyDestroyLocked(*dsRef.state, dsRef.binding, dsRef.elem, *this);
	}
}

// API
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
		VkDevice                                    device,
		const VkBufferCreateInfo*                   pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkBuffer*                                   pBuffer) {
	auto& dev = getData<Device>(device);

	auto nci = *pCreateInfo;

	// Needed so we can copy from it for show its contents.
	// AFAIK this should always be supported, for all buffers.
	nci.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	// NOTE: needed for our own operations on the buffer. Might be better to
	// properly acquire/release it instead though, this might have
	// a performance impact.
	auto concurrent = false;
	if(dev.usedQueueFamilyIndices.size() > 1 && nci.sharingMode != VK_SHARING_MODE_CONCURRENT) {
		nci.sharingMode = VK_SHARING_MODE_CONCURRENT;
		nci.queueFamilyIndexCount = u32(dev.usedQueueFamilyIndices.size());
		nci.pQueueFamilyIndices = dev.usedQueueFamilyIndices.data();
		concurrent = true;
	}

	auto res = dev.dispatch.CreateBuffer(device, &nci, pAllocator, pBuffer);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& buf = dev.buffers.add(*pBuffer);
	buf.objectType = VK_OBJECT_TYPE_BUFFER;
	buf.dev = &dev;
	buf.ci = *pCreateInfo;
	buf.handle = *pBuffer;
	buf.concurrentHooked = concurrent;

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		const VkAllocationCallbacks*                pAllocator) {
	if(!buffer) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.buffers.mustErase(buffer);
	dev.dispatch.DestroyBuffer(device, buffer, pAllocator);
}

void bindBufferMemory(Device& dev, const VkBindBufferMemoryInfo& bind) {
	auto& buf = dev.buffers.get(bind.buffer);
	auto& mem = dev.deviceMemories.get(bind.memory);

	dlg_assert(!buf.memory);

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, bind.buffer, &memReqs);

	buf.memory = &mem;
	buf.allocationOffset = bind.memoryOffset;
	buf.allocationSize = memReqs.size;

	{
		// access to the given memory must be internally synced
		std::lock_guard lock(dev.mutex);
		// mem.allocations.insert(&buf);
		mem.allocations.push_back(&buf);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		VkDeviceMemory                              memory,
		VkDeviceSize                                memoryOffset) {
	auto& dev = getData<Device>(device);
	bindBufferMemory(dev, {{}, {}, buffer, memory, memoryOffset});
	return dev.dispatch.BindBufferMemory(device, buffer, memory, memoryOffset);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(
		VkDevice                                    device,
		uint32_t                                    bindInfoCount,
		const VkBindBufferMemoryInfo*               pBindInfos) {
	auto& dev = getData<Device>(device);
	for(auto i = 0u; i < bindInfoCount; ++i) {
		auto& bind = pBindInfos[i];
		bindBufferMemory(dev, bind);
	}

	return dev.dispatch.BindBufferMemory2(device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(
		VkDevice                                    device,
		const VkBufferViewCreateInfo*               pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkBufferView*                               pView) {
	auto& dev = getData<Device>(device);
	auto res = dev.dispatch.CreateBufferView(device, pCreateInfo, pAllocator, pView);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& view = dev.bufferViews.add(*pView);
	view.dev = &dev;
	view.objectType = VK_OBJECT_TYPE_BUFFER_VIEW;
	view.handle = *pView;
	view.buffer = &dev.buffers.get(pCreateInfo->buffer);
	view.ci = *pCreateInfo;

	{
		std::lock_guard lock(dev.mutex);
		view.buffer->views.push_back(&view);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(
		VkDevice                                    device,
		VkBufferView                                bufferView,
		const VkAllocationCallbacks*                pAllocator) {
	if(!bufferView) {
		return;
	}

	auto& dev = getData<Device>(device);
	dev.bufferViews.mustErase(bufferView);
	dev.dispatch.DestroyBufferView(device, bufferView, pAllocator);
}

} // namespace vil
