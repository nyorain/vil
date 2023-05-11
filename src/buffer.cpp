#include <buffer.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <data.hpp>
#include <ds.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>

namespace vil {

VkDeviceSize evalRange(VkDeviceSize fullSize, VkDeviceSize offset, VkDeviceSize range) {
	dlg_assert(offset <= fullSize);
	return range == VK_WHOLE_SIZE ? fullSize - offset : range;
}

Buffer* bufferAtInternal(const decltype(Device::bufferAddresses)& bufferAddresses,
		VkDeviceAddress address) {
	auto it = bufferAddresses.upper_bound(address);

	// NOTE: for aliased buffers, this might degenerate into linear search,
	// but we'll only ever have to iterate so many elements as elements
	// overlap via aliasing so it should be fine.
	// A better algorithmic solution would be to use an interval
	// tree (e.g. augmented RB-Tree) but I couldn't find a good implementation
	// to pull in easily and now it seems overkill anyways.
	while(it != bufferAddresses.begin()) {
		--it;
		dlg_assert((*it)->deviceAddress <= address);
		auto end = (*it)->deviceAddress + (*it)->ci.size;
		if(end > address) {
			return *it;
		}
	}

	// When we didn't find anything we had to iterate *all* buffer addresses.
	// This would be a major performance concern but we will most likely
	// crash in this case anyways so we don't care.
	return nullptr;
}

Buffer& bufferAtLocked(Device& dev, VkDeviceAddress address) {
	assertOwnedOrShared(dev.mutex);
	auto* buf = bufferAtInternal(dev.bufferAddresses, address);
	if(!buf) {
		dlg_error("Unknown buffer device address {}", address);
		throw std::invalid_argument("Invalid buffer device address");
	}

	return *buf;
}

Buffer& bufferAt(Device& dev, VkDeviceAddress address) {
	auto lock = std::shared_lock(dev.mutex);
	return bufferAtLocked(dev, address);
}

// Classes
void Buffer::onApiDestroy() {
	MemoryResource::onApiDestroy();

	std::lock_guard lock(dev->mutex);
	if(ci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		auto num = dev->bufferAddresses.erase(this);
		dlg_assert(num == 1u);
	}

	for(auto* view : this->views) {
		view->buffer = nullptr;
	}
}

Buffer::~Buffer() {
	if(!dev) {
		return;
	}

	dlg_assert(DebugStats::get().aliveBuffers > 0);
	--DebugStats::get().aliveBuffers;
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

void retrieveDeviceAddress(Buffer& buf) {
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

void checkDeviceAddress(Buffer& buf) {
	if(buf.ci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		retrieveDeviceAddress(buf);
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
	buf.memObjectType = VK_OBJECT_TYPE_BUFFER;
	buf.dev = &dev;
	buf.ci = *pCreateInfo;
	buf.handle = *pBuffer;
	buf.concurrentHooked = concurrent;

	if(buf.ci.flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
		buf.memory = SparseMemoryState{};

		// for non-sparse buffer, the device address can only be retrieved when
		// its bound to memory, but for non-sparse buffers it can be retrieved
		// immediately
		checkDeviceAddress(buf);
	}

	constexpr auto sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	auto* externalMem = findChainInfo<VkExternalMemoryBufferCreateInfo, sType>(nci);
	if(externalMem && externalMem->handleTypes) {
		buf.externalMemory = true;
	}

	++DebugStats::get().aliveBuffers;

	*pBuffer = castDispatch<VkBuffer>(buf);
	dev.buffers.mustEmplace(std::move(bufPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(
		VkDevice                                    device,
		VkBuffer                                    buffer,
		const VkAllocationCallbacks*                pAllocator) {
	if(!buffer) {
		return;
	}

	auto& dev = mustMoveUnsetKeepAlive<&Device::keepAliveBuffers>(device, buffer);
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

	// find required size
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf.handle, &memReqs);

	// access to the given memory and buffer must be internally synced
	std::lock_guard lock(dev.mutex);
	dlg_assert(buf.memory.index() == 0u);
	auto& memBind = std::get<0>(buf.memory);

	dlg_assert(!memBind.memory);
	dlg_assert(memBind.memState == FullMemoryBind::State::unbound);

	memBind.memory = &mem;
	memBind.memOffset = offset;
	memBind.memSize = memReqs.size;
	memBind.memState = FullMemoryBind::State::bound;
	memBind.resource = &buf;

	mem.allocations.insert(&memBind);
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
		dlg_assert(bind.buffer);

		auto& buf = get(dev, bind.buffer);

		fwd[i] = bind;
		fwd[i].buffer = buf.handle;

		// could be VK_NULL_HANDLE for extensions
		// e.g. the case for BindImageMemory2, we add this branch
		// here defensively
		if(bind.memory) {
			auto& mem = get(dev, bind.memory);
			bindBufferMemory(buf, mem, bind.memoryOffset);
			fwd[i].memory = mem.handle;
		}
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
	view.handle = *pView;
	view.buffer = &buf;
	view.ci = *pCreateInfo;

	{
		std::lock_guard lock(dev.mutex);
		view.buffer->views.push_back(&view);
	}

	*pView = castDispatch<VkBufferView>(view);
	dev.bufferViews.mustEmplace(std::move(viewPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(
		VkDevice                                    device,
		VkBufferView                                bufferView,
		const VkAllocationCallbacks*                pAllocator) {
	if(!bufferView) {
		return;
	}

	auto& dev = mustMoveUnsetKeepAlive<&Device::keepAliveBufferViews>(device, bufferView);
	dev.dispatch.DestroyBufferView(dev.handle, bufferView, pAllocator);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(
		VkDevice                                    device,
		const VkBufferDeviceAddressInfo*            pInfo) {
	auto& buf = get(device, pInfo->buffer);
	dlg_assert(buf.ci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

	auto fwd = *pInfo;
	fwd.buffer = buf.handle;
	auto ret = buf.dev->dispatch.GetBufferDeviceAddressKHR(buf.dev->handle, &fwd);

	// This is a sign of a serious problem, we might have missed to retrieve
	// it at the right point. We try to insert it now, in the hope that
	// this avoid further problems
	if(ret != buf.deviceAddress) {
		dlg_error("Inconsistent/Unknown device address retrieved");

		if(buf.deviceAddress) {
			// We already retrieved a (different) device addres?!
			// This makes no sense at all, we can't even try to fix the
			// issue in this case.
			dlg_error("Different device address retrieved?!");
		} else {
			retrieveDeviceAddress(buf);
			dlg_assert(ret == buf.deviceAddress);
		}
	}

	return ret;
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
