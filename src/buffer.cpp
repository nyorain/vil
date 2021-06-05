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

Buffer& bufferAtLocked(Device& dev, VkDeviceAddress address) {
	assertOwnedOrShared(dev.mutex);

	auto [begin, end] = dev.bufferAddresses.equal_range(address);
	dlg_assertm(begin != end, "Invalid VkDeviceAddress: couldn't find buffer");
	if(std::distance(begin, end) > 1) {
		dlg_trace("More than one buffer found for device address; will have to guess");
	}

	Buffer* best = nullptr;
	while(begin != end) {
		if(!best || (best->deviceAddress + best->ci.size - address) <
				((*begin)->deviceAddress + (*begin)->ci.size - address)) {
			best = *begin;
		}

		++begin;
	}

	dlg_assert(best);
	return *best;
}

Buffer& bufferAt(Device& dev, VkDeviceAddress address) {
	auto lock = std::shared_lock(dev.mutex);
	return bufferAtLocked(dev, address);
}

// Classes
Buffer::~Buffer() {
	if(!dev) {
		return;
	}

	dlg_assert(DebugStats::get().aliveBuffers > 0);
	--DebugStats::get().aliveBuffers;

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

	auto res = dev.dispatch.CreateBuffer(dev.handle, &nci, pAllocator, pBuffer);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto bufPtr = IntrusivePtr<Buffer>(new Buffer);
	auto& buf = *bufPtr;
	buf.objectType = VK_OBJECT_TYPE_BUFFER;
	buf.dev = &dev;
	buf.ci = *pCreateInfo;
	buf.handle = *pBuffer;
	buf.concurrentHooked = concurrent;

	++DebugStats::get().aliveBuffers;

	*pBuffer = castDispatch<VkBuffer>(buf);
	dev.buffers.mustEmplace(*pBuffer, std::move(bufPtr));

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
	IntrusivePtr<Buffer> ptr;

	{
		std::lock_guard lock(dev.mutex);
		ptr = dev.buffers.mustMoveLocked(buffer);

		if(ptr->ci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
			auto num = dev.bufferAddresses.erase(ptr.get());
			dlg_assert(num == 1u);
		}

		buffer = ptr->handle;
		ptr->handle = {};
	}

	dev.dispatch.DestroyBuffer(dev.handle, buffer, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		VkMemoryRequirements*                       pMemoryRequirements) {
	auto& buf = get(device, buffer);
	auto& dev = *buf.dev;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf.handle, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(
		VkDevice                                    device,
		const VkBufferMemoryRequirementsInfo2*      pInfo,
		VkMemoryRequirements2*                      pMemoryRequirements) {
	auto& buf = get(device, pInfo->buffer);
	auto& dev = *buf.dev;

	auto copy = *pInfo;
	copy.buffer = buf.handle;

	dev.dispatch.GetBufferMemoryRequirements2(dev.handle, &copy, pMemoryRequirements);
}

void bindBufferMemory(Buffer& buf, DeviceMemory& mem, VkDeviceSize offset) {
	auto& dev = *buf.dev;
	dlg_assert(!buf.memory);

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf.handle, &memReqs);

	// access to the given memory must be internally synced
	std::lock_guard lock(dev.mutex);
	buf.memory = &mem;
	buf.allocationOffset = offset;
	buf.allocationSize = memReqs.size;

	mem.allocations.push_back(&buf);
}

void checkDeviceAddress(Buffer& buf) {
	if(buf.ci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		auto& dev = *buf.dev;
		dlg_assert(dev.dispatch.GetBufferDeviceAddress);
		dlg_assert(!buf.deviceAddress);

		VkBufferDeviceAddressInfo info {};
		info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		info.buffer = buf.handle;
		auto address = dev.dispatch.GetBufferDeviceAddress(dev.handle, &info);

		std::lock_guard lock(dev.mutex);
		buf.deviceAddress = address;
		dev.bufferAddresses.insert(&buf);
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

	auto res = buf.dev->dispatch.BindBufferMemory(buf.dev->handle,
		buf.handle, memory, memoryOffset);
	if(res != VK_SUCCESS) {
		return res;
	}

	checkDeviceAddress(buf);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(
		VkDevice                                    device,
		uint32_t                                    bindInfoCount,
		const VkBindBufferMemoryInfo*               pBindInfos) {
	auto& dev = getDevice(device);

	ThreadMemScope memScope;
	auto fwd = memScope.alloc<VkBindBufferMemoryInfo>(bindInfoCount);
	for(auto i = 0u; i < bindInfoCount; ++i) {
		auto& bind = pBindInfos[i];
		auto& buf = get(dev, bind.buffer);
		auto& mem = get(dev, bind.memory);
		bindBufferMemory(buf, mem, bind.memoryOffset);

		fwd[i] = bind;
		fwd[i].buffer = buf.handle;
		fwd[i].memory = mem.handle;
	}

	auto res = dev.dispatch.BindBufferMemory2(dev.handle, u32(fwd.size()), fwd.data());
	if(res != VK_SUCCESS) {
		return res;
	}

	for(auto i = 0u; i < bindInfoCount; ++i) {
		auto& bind = pBindInfos[i];
		auto& buf = get(dev, bind.buffer);
		checkDeviceAddress(buf);
	}

	return VK_SUCCESS;
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

	auto res = dev.dispatch.CreateBufferView(dev.handle, &ci, pAllocator, pView);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto viewPtr = IntrusivePtr<BufferView>(new BufferView());
	auto& view = *viewPtr;
	view.dev = &dev;
	view.objectType = VK_OBJECT_TYPE_BUFFER_VIEW;
	view.handle = *pView;
	view.buffer = &buf;
	view.ci = *pCreateInfo;

	{
		std::lock_guard lock(dev.mutex);
		view.buffer->views.push_back(&view);
	}

	*pView = castDispatch<VkBufferView>(view);
	dev.bufferViews.mustEmplace(*pView, std::move(viewPtr));

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
	IntrusivePtr<BufferView> ptr;

	{
		std::lock_guard lock(dev.mutex);
		ptr = dev.bufferViews.mustMoveLocked(bufferView);
		bufferView = ptr->handle;
		ptr->handle = {}; // mark as destroyed
	}

	dev.dispatch.DestroyBufferView(dev.handle, bufferView, pAllocator);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(
		VkDevice                                    device,
		const VkBufferDeviceAddressInfo*            pInfo) {
	auto& buf = get(device, pInfo->buffer);
	auto fwd = *pInfo;
	fwd.buffer = buf.handle;
	return buf.dev->dispatch.GetBufferDeviceAddressKHR(buf.dev->handle, &fwd);
}

// NOTE: these functions are specifically designed for layers that capture
// stuff. But we don't actually need it since we capture/show everything
// in-process.
VKAPI_ATTR uint64_t VKAPI_CALL GetBufferOpaqueCaptureAddress(
		VkDevice                                    device,
		const VkBufferDeviceAddressInfo*            pInfo) {
	auto& buf = get(device, pInfo->buffer);
	auto fwd = *pInfo;
	fwd.buffer = buf.handle;
	return buf.dev->dispatch.GetBufferOpaqueCaptureAddressKHR(buf.dev->handle, &fwd);
}

VKAPI_ATTR uint64_t VKAPI_CALL GetDeviceMemoryOpaqueCaptureAddress(
		VkDevice                                    device,
		const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo) {
	auto& mem = get(device, pInfo->memory);
	auto fwd = *pInfo;
	fwd.memory = mem.handle;
	return mem.dev->dispatch.GetDeviceMemoryOpaqueCaptureAddressKHR(mem.dev->handle, &fwd);
}

} // namespace vil
