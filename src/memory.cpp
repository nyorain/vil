#include <memory.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>

namespace vil {

bool MemoryBind::CmpByMemOffset::operator()(const MemoryBind& a, const MemoryBind& b) const {
	dlg_assert(a.memory == b.memory);

	if(a.memOffset != b.memOffset) {
		return a.memOffset < b.memOffset;
	}

	if(a.memSize != b.memSize) {
		return a.memSize < b.memSize;
	}

	// arbitrary ordering just for uniqueness
	// NOTE: sorting by resource alone is not enough due to
	//   inner-resource aliasing for sparse resources
	return &a < &b;
}

bool OpaqueSparseMemoryBind::Cmp::operator()(const OpaqueSparseMemoryBind& a,
		const OpaqueSparseMemoryBind& b) const {
	dlg_assert(a.resource == b.resource);

	if(a.resourceOffset != b.resourceOffset) {
		return a.resourceOffset < b.resourceOffset;
	}

	return false;

	// if(a.memSize != b.memSize) {
	// 	return a.memSize < b.memSize;
	// }

	// arbitrary ordering just for uniqueness
	// return &a < &b;
}

bool ImageSparseMemoryBind::Cmp::operator()(const ImageSparseMemoryBind& a,
		const ImageSparseMemoryBind& b) const {
	dlg_assert(a.resource == b.resource);

	if(a.subres.aspectMask != b.subres.aspectMask) {
		return a.subres.aspectMask < b.subres.aspectMask;
	}
	if(a.subres.mipLevel != b.subres.mipLevel) {
		return a.subres.mipLevel < b.subres.mipLevel;
	}
	if(a.subres.arrayLayer != b.subres.arrayLayer) {
		return a.subres.arrayLayer < b.subres.arrayLayer;
	}

	if(a.offset.z != b.offset.z) return a.offset.z < b.offset.z;
	if(a.offset.y != b.offset.y) return a.offset.y < b.offset.y;
	if(a.offset.x != b.offset.x) return a.offset.x < b.offset.x;

	// Don't compare on size by design here
	// If we land here, all the offsets/subres is the same.
	// There can't multiple bindings (with different size) for the
	// same address.

	// arbitrary ordering just for uniqueness
	// return &a < &b;
	return false;
}

void MemoryResource::onApiDestroy() {
	dlg_assert(dev);
	std::lock_guard lock(dev->mutex);

	// unregister at memory
	std::visit(Visitor{
		[](FullMemoryBind& bind) {
			dlg_assertm(!!bind.memory ==
				(bind.memState == FullMemoryBind::State::bound),
				"Inconsistent FullMemoryBind state");
			if(bind.memory) {
				dlg_assert(bind.memState == FullMemoryBind::State::bound);
				bind.memory->allocations.erase(&bind);
				bind.memory = {};
				bind.memOffset = {};
				bind.memSize = {};
				bind.memState = FullMemoryBind::State::resourceDestroyed;
			}
		},
		[](SparseMemoryState& mem) {
			for(auto& bind : mem.opaqueBinds) {
				if(bind.memory) {
					bind.memory->allocations.erase(&bind);
				}
			}
			for(auto& bind : mem.imageBinds) {
				if(bind.memory) {
					bind.memory->allocations.erase(&bind);
				}
			}

			mem.opaqueBinds.clear();
			mem.imageBinds.clear();
		}
	}, memory);

	notifyApiHandleDestroyedLocked(*dev, *this, memObjectType);
}

void DeviceMemory::onApiDestroy() {
	std::lock_guard lock(dev->mutex);

	for(auto* bind : allocations) {
		dlg_assert(bind->resource);
		auto& res = *bind->resource;

		std::visit(Visitor{
			[&](FullMemoryBind& bind) {
				dlg_assert(bind.memState == FullMemoryBind::State::bound);
				dlg_assert(bind.memory == this);
				bind.memory = nullptr;
				bind.memState = FullMemoryBind::State::memoryDestroyed;
				bind.memOffset = 0u;
				bind.memSize = 0u;

				notifyMemoryResourceInvalidatedLocked(*dev, res);
			},
			[&](SparseMemoryState& mem) {
				(void) mem;

				// NOTE: we explicitly don't remove the allocation objects
				//   here. We need to know (e.g. in gui code) if a resource
				//   has sparse memory objects bound to it that were destroyed.
				//   Per API spec they still need to be explicitly unbound,
				//   otherwise using the resource is invalid
				dlg_assert(bind->memory == this);
				auto& dst = *const_cast<MemoryBind*>(bind);
				dst.memory = nullptr;
				dst.memOffset = 0u;

				notifyMemoryResourceInvalidatedLocked(*dev, *dst.resource);
			}
		}, res.memory);
	}

	allocations.clear();
}

// Memory
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
		VkDevice                                    device,
		const VkMemoryAllocateInfo*                 pAllocateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDeviceMemory*                             pMemory) {
	auto& dev = getDevice(device);

	checkInitWindow(dev);

	ThreadMemScope memScope;
	auto chainCopy = copyChainLocal(memScope, pAllocateInfo->pNext);
	auto allocInfo = *pAllocateInfo;
	allocInfo.pNext = chainCopy;

	// unwrap handles in VkMemoryDedicatedAllocateInfo
	auto* dedicatedv = findChainInfo2<VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO>(chainCopy);
	if(dedicatedv) {
		auto* dedicated = static_cast<VkMemoryDedicatedAllocateInfo*>(dedicatedv);
		dlg_assert(!dedicated->buffer || !dedicated->image);
		if(dedicated->buffer) {
			auto& buf = get(dev, dedicated->buffer);
			dedicated->buffer = buf.handle;
		} else if(dedicated->image) {
			auto& img = get(dev, dedicated->image);
			dedicated->image = img.handle;
		}
	}

	auto res = dev.dispatch.AllocateMemory(dev.handle, &allocInfo, pAllocator, pMemory);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto memPtr = IntrusivePtr<DeviceMemory>(new DeviceMemory());
	auto& memory = *memPtr;
	memory.dev = &dev;
	memory.handle = *pMemory;
	memory.typeIndex = pAllocateInfo->memoryTypeIndex;
	memory.size = pAllocateInfo->allocationSize;

	*pMemory = castDispatch<VkDeviceMemory>(memory);
	dev.deviceMemories.mustEmplace(memory.handle, std::move(memPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		const VkAllocationCallbacks*                pAllocator) {
	if(!memory) {
		return;
	}

	auto& dev = *mustMoveUnset(device, memory)->dev;
	dev.dispatch.FreeMemory(dev.handle, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		VkDeviceSize                                offset,
		VkDeviceSize                                size,
		VkMemoryMapFlags                            flags,
		void**                                      ppData) {
	auto& mem = get(device, memory);
	auto res = mem.dev->dispatch.MapMemory(mem.dev->handle, mem.handle,
		offset, size, flags, ppData);
	if(res != VK_SUCCESS) {
		return res;
	}

	std::lock_guard lock(mem.dev->mutex); // TODO PERF
	dlg_assert(!mem.map);
	mem.map = *ppData;
	mem.mapOffset = offset;
	mem.mapSize = size;

	return res;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory) {
	auto& mem = get(device, memory);

	{
		std::lock_guard lock(mem.dev->mutex); // TODO PERF
		dlg_assert(mem.map);
		mem.map = nullptr;
		mem.mapOffset = 0u;
		mem.mapSize = 0u;
	}

	mem.dev->dispatch.UnmapMemory(mem.dev->handle, mem.handle);
}

VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(
		VkDevice                                    device,
		uint32_t                                    memoryRangeCount,
		const VkMappedMemoryRange*                  pMemoryRanges) {
	auto& dev = getDevice(device);
	ThreadMemScope memScope;
	auto fwd = memScope.alloc<VkMappedMemoryRange>(memoryRangeCount);
	for(auto i = 0u; i < memoryRangeCount; ++i) {
		fwd[i] = *pMemoryRanges;
		fwd[i].memory = get(dev, fwd[i].memory).handle;
	}

	return dev.dispatch.FlushMappedMemoryRanges(dev.handle, u32(fwd.size()), fwd.data());
}

VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(
		VkDevice                                    device,
		uint32_t                                    memoryRangeCount,
		const VkMappedMemoryRange*                  pMemoryRanges) {
	auto& dev = getDevice(device);
	ThreadMemScope memScope;
	auto fwd = memScope.alloc<VkMappedMemoryRange>(memoryRangeCount);
	for(auto i = 0u; i < memoryRangeCount; ++i) {
		fwd[i] = *pMemoryRanges;
		fwd[i].memory = get(dev, fwd[i].memory).handle;
	}

	return dev.dispatch.InvalidateMappedMemoryRanges(dev.handle, u32(fwd.size()), fwd.data());
}

VKAPI_ATTR void VKAPI_CALL GetDeviceMemoryCommitment(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		VkDeviceSize*                               pCommittedMemoryInBytes) {
	auto& mem = get(device, memory);
	mem.dev->dispatch.GetDeviceMemoryCommitment(mem.dev->handle, mem.handle,
		pCommittedMemoryInBytes);
}

// Calls the given callback for every resource overlapping the given
// range of mem[off, off + size)
template<typename F>
void forEachOverlappingResourceLocked(const DeviceMemory& mem,
		VkDeviceSize off, VkDeviceSize size, F&& callback) {
	assertOwned(mem.dev->mutex);

	MemoryBind tester {};
	tester.memOffset = off;
	tester.memory = const_cast<DeviceMemory*>(&mem);

	auto end = off + size;
	auto it = mem.allocations.lower_bound(&tester);
	while(it != mem.allocations.end() && (*it)->memOffset < end) {
		callback(**it);
	}
}

} // namespace vil
