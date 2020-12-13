#include "buffer.hpp"
#include "data.hpp"
#include "ds.hpp"

namespace fuen {

// Classes
Buffer::~Buffer() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);
	for(auto* view : this->views) {
		view->buffer = nullptr;
	}

	while(!this->descriptors.empty()) {
		auto ref = this->descriptors[0];
		dlg_assert(ref.ds->getBuffer(ref.binding, ref.elem) == this);
		ref.ds->invalidateLocked(ref.binding, ref.elem);
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

	// Don't use for-loop as the descriptors unregister themselves
	while(!this->descriptors.empty()) {
		auto ref = this->descriptors[0];
		dlg_assert(ref.ds->getBufferView(ref.binding, ref.elem) == this);
		ref.ds->invalidateLocked(ref.binding, ref.elem);
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
	nci.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	// TODO: needed for our own operations on the buffer. We should
	// properly acquire/release it instead though, this might have
	// a performance impact.
	if(dev.usedQueueFamilyIndices.size() > 1) {
		nci.sharingMode = VK_SHARING_MODE_CONCURRENT;
		nci.queueFamilyIndexCount = dev.usedQueueFamilyIndices.size();
		nci.pQueueFamilyIndices = dev.usedQueueFamilyIndices.data();
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

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		const VkAllocationCallbacks*                pAllocator) {
	auto& dev = getData<Device>(device);
	dev.buffers.mustErase(buffer);
	dev.dispatch.DestroyBuffer(device, buffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		VkDeviceMemory                              memory,
		VkDeviceSize                                memoryOffset) {
	auto& dev = getData<Device>(device);
	auto& buf = dev.buffers.get(buffer);
	auto& mem = dev.deviceMemories.get(memory);

	dlg_assert(!buf.memory);

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(device, buffer, &memReqs);

	buf.memory = &mem;
	buf.allocationOffset = memoryOffset;
	buf.allocationSize = memReqs.size;

	{
		// access to the given memory must be internally synced
		std::lock_guard lock(dev.mutex);
		mem.allocations.insert(&buf);
	}

	return dev.dispatch.BindBufferMemory(device, buffer, memory, memoryOffset);
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
	auto& dev = getData<Device>(device);
	dev.bufferViews.mustErase(bufferView);
	dev.dispatch.DestroyBufferView(device, bufferView, pAllocator);
}

} // namespace fuen
