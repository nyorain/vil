#include <buffer.hpp>
#include <device.hpp>
#include <data.hpp>
#include <ds.hpp>
#include <threadContext.hpp>

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
}

// API
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
		VkDevice                                    device,
		const VkBufferCreateInfo*                   pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkBuffer*                                   pBuffer) {
	auto& dev = getDevice(device);

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

	VkBuffer handle;
	auto res = dev.dispatch.CreateBuffer(dev.handle, &nci, pAllocator, &handle);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& bufPtr = dev.buffers.addPtr(*pBuffer);
	auto& buf = *bufPtr;
	buf.objectType = VK_OBJECT_TYPE_BUFFER;
	buf.dev = &dev;
	buf.ci = *pCreateInfo;
	buf.handle = handle;
	buf.concurrentHooked = concurrent;

	*pBuffer = castDispatch<VkBuffer>(dev, *bufPtr.wrapped());
	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		const VkAllocationCallbacks*                pAllocator) {
	if(!buffer) {
		return;
	}

	auto& dev = getDevice(device);
	VkBuffer handle;

	{
		std::lock_guard lock(dev.mutex);
		auto ptr = dev.buffers.mustMoveLocked(buffer);
		handle = ptr->handle;
		ptr->handle = {};
	}

	dev.dispatch.DestroyBuffer(dev.handle, handle, pAllocator);
}

void bindBufferMemory(Buffer& buf, DeviceMemory& mem, VkDeviceSize offset) {
	auto& dev = *buf.dev;
	dlg_assert(!buf.memory);

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf.handle, &memReqs);

	{
		// access to the given memory must be internally synced
		std::lock_guard lock(dev.mutex);
		buf.memory = &mem;
		buf.allocationOffset = offset;
		buf.allocationSize = memReqs.size;

		mem.allocations.push_back(&buf);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		VkDeviceMemory                              memory,
		VkDeviceSize                                memoryOffset) {
	auto& buf = get(device, buffer);
	auto& mem = get(*buf.dev, memory);
	bindBufferMemory(buf, mem, memoryOffset);
	return buf.dev->dispatch.BindBufferMemory(buf.dev->handle,
		buf.handle, memory, memoryOffset);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(
		VkDevice                                    device,
		uint32_t                                    bindInfoCount,
		const VkBindBufferMemoryInfo*               pBindInfos) {
	auto& dev = getDevice(device);
	auto fwd = LocalVector<VkBindBufferMemoryInfo>(bindInfoCount);
	for(auto i = 0u; i < bindInfoCount; ++i) {
		auto& bind = pBindInfos[i];
		auto& buf = get(dev, bind.buffer);
		auto& mem = get(dev, bind.memory);
		bindBufferMemory(buf, mem, bind.memoryOffset);

		fwd[i] = bind;
		fwd[i].buffer = buf.handle;
		fwd[i].memory = mem.handle;
	}

	return dev.dispatch.BindBufferMemory2(dev.handle, u32(fwd.size()), fwd.data());
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(
		VkDevice                                    device,
		const VkBufferViewCreateInfo*               pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkBufferView*                               pView) {
	auto& buf = get(device, pCreateInfo->buffer);
	auto& dev = *buf.dev;

	auto ci = *pCreateInfo;
	ci.buffer = buf.handle;

	VkBufferView handle;
	auto res = dev.dispatch.CreateBufferView(dev.handle, &ci, pAllocator, &handle);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& viewPtr = dev.bufferViews.addPtr(handle);
	auto& view = *viewPtr;
	view.dev = &dev;
	view.objectType = VK_OBJECT_TYPE_BUFFER_VIEW;
	view.handle = handle;
	view.buffer = &buf;
	view.ci = *pCreateInfo;

	{
		std::lock_guard lock(dev.mutex);
		view.buffer->views.push_back(&view);
	}

	*pView = castDispatch<VkBufferView>(dev, *viewPtr.wrapped());
	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(
		VkDevice                                    device,
		VkBufferView                                bufferView,
		const VkAllocationCallbacks*                pAllocator) {
	if(!bufferView) {
		return;
	}

	auto& dev = getDevice(device);
	VkBufferView handle;

	{
		std::lock_guard lock(dev.mutex);
		auto ptr = dev.bufferViews.mustMoveLocked(bufferView);
		handle = ptr->handle;
		ptr->handle = {}; // mark as destroyed
	}

	dev.dispatch.DestroyBufferView(dev.handle, handle, pAllocator);
}

} // namespace vil
