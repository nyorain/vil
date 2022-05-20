#include <memory.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <image.hpp>
#include <buffer.hpp>
#include <threadContext.hpp>
#include <cow.hpp>
#include <util/util.hpp>

namespace vil {

// MemoryResource
MemoryResource::~MemoryResource() {
	if(!dev) {
		return;
	}

	// If this resource had memory assigned, remove it from the memories
	// list of resources.
	std::lock_guard lock(dev->mutex);
	if(memory) {
		dlg_assert(!memoryDestroyed);

		auto it = std::lower_bound(memory->allocations.begin(), memory->allocations.end(),
			*this, cmpMemRes);

		// NOTE: we can't just iterate it but have to do a linear
		// search until we find *this since memory->allocations can contain
		// aliased memory resources
		auto found = false;
		while(it != memory->allocations.end()) {
			if(*it == this) {
				found = true;
				memory->allocations.erase(it);
				break;
			}

			++it;
		}

		dlg_assert(found);
	}
}

// DeviceMemory
DeviceMemory::~DeviceMemory() {
	if(!dev) {
		return;
	}

	std::lock_guard lock(dev->mutex);

	invalidateCbsLocked();
	notifyDestructionLocked(*dev, *this, VK_OBJECT_TYPE_DEVICE_MEMORY);

	// NOTE that we temporarily invalidate the ordering invariant of
	// this->allocations. But since we own the mutex and this object
	// is to be destroyed any moment anyways (and we are not calling
	// any other functions in between) this isn't a problem.
	for(auto* res : allocations) {
		dlg_assert(!res->memoryDestroyed);
		dlg_assert(res->memory == this);
		res->memory = nullptr;
		res->memoryDestroyed = true;
		res->allocationOffset = 0u;
		res->allocationSize = 0u;
	}
}

// Memory
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(
		VkDevice                                    device,
		const VkMemoryAllocateInfo*                 pAllocateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDeviceMemory*                             pMemory) {
	auto& dev = getDevice(device);

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

	auto memPtr = std::make_unique<DeviceMemory>();
	auto& memory = *memPtr;
	memory.objectType = VK_OBJECT_TYPE_DEVICE_MEMORY;
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

	auto& dev = getDevice(device);
	auto handle = dev.deviceMemories.mustMove(memory)->handle;
	dev.dispatch.FreeMemory(dev.handle, handle, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(
		VkDevice                                    device,
		VkDeviceMemory                              memory,
		VkDeviceSize                                offset,
		VkDeviceSize                                size,
		VkMemoryMapFlags                            flags,
		void**                                      ppData) {
	auto& mem = get(device, memory);
	const auto res = mem.dev->dispatch.MapMemory(mem.dev->handle, mem.handle,
		offset, size, flags, ppData);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& dev = *mem.dev;
	if(size == VK_WHOLE_SIZE) {
		size = mem.size - offset;
	}

	std::unique_lock lock(dev.mutex); // TODO PERF
	dlg_assert(!mem.map);
	mem.map = *ppData;
	mem.mapOffset = offset;
	mem.mapSize = size;

	// resolve cows of mapped resources
	const auto mapEnd = mem.mapOffset + mem.mapSize;

	std::optional<CowResolveOp> cowResolve;
	auto getCowResolve = [&]() -> decltype(auto) {
		if(cowResolve) {
			return *cowResolve;
		}

		// create
		auto& cr = cowResolve.emplace();
		initLocked(dev, cr);
		return cr;
	};

	for(auto* alloc : mem.allocations) {
		if(alloc->allocationOffset + alloc->allocationSize <= offset) {
			// no overlap
			continue;
		}

		if(alloc->allocationOffset >= mapEnd) {
			// break here, allocations are sorted by offset
			break;
		}

		if(alloc->objectType == VK_OBJECT_TYPE_BUFFER) {
			auto& img = static_cast<Image&>(*alloc);
			for(auto& cow : img.cows) {
				recordResolve(dev, getCowResolve(), *cow);
			}
			img.cows.clear();
		} else if(alloc->objectType == VK_OBJECT_TYPE_IMAGE) {
			auto& buf = static_cast<Buffer&>(*alloc);
			for(auto& cow : buf.cows) {
				recordResolve(dev, getCowResolve(), *cow);
			}
			buf.cows.clear();
		} else {
			dlg_error("unreachable");
		}
	}

	if(cowResolve) {
		auto& cr = *cowResolve;
		++cr.queue->submissionCounter;

		VkSubmitInfo si {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1u;
		si.pCommandBuffers = &cr.cb;

		// add full sync
		dlg_assert(dev.doFullSync);

		ThreadMemScope tms;
		auto maxWaitSemCount = dev.queues.size();
		auto waitSems = tms.alloc<VkSemaphore>(maxWaitSemCount);
		auto waitStages = tms.alloc<VkPipelineStageFlags>(maxWaitSemCount);
		auto waitVals = tms.alloc<u64>(maxWaitSemCount);
		auto waitSemCount = 0u;

		for(auto& pqueue : dev.queues) {
			auto& queue = *pqueue;
			if(&queue == cr.queue) {
				continue;
			}

			// check if pending submissions on this queue
			u64 finishedID;
			dev.dispatch.GetSemaphoreCounterValue(dev.handle,
				queue.submissionSemaphore, &finishedID);
			if(finishedID == queue.submissionCounter) {
				continue;
			}

			waitVals[waitSemCount] = queue.submissionCounter;
			waitSems[waitSemCount] = queue.submissionSemaphore;
			waitStages[waitSemCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			++waitSemCount;
		}

		si.pWaitDstStageMask = waitStages.data();
		si.pWaitSemaphores = waitSems.data();
		si.waitSemaphoreCount = waitSemCount;
		si.signalSemaphoreCount = 1u;
		si.pSignalSemaphores = &cr.queue->submissionSemaphore;

		VkTimelineSemaphoreSubmitInfo tsInfo {};
		tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		tsInfo.waitSemaphoreValueCount = waitSemCount;
		tsInfo.pWaitSemaphoreValues = waitVals.data();
		tsInfo.signalSemaphoreValueCount = 1u;
		tsInfo.pSignalSemaphoreValues = &cr.queue->submissionCounter;

		si.pNext = &tsInfo;

		auto res = dev.dispatch.QueueSubmit(cr.queue->handle, 1u, &si, cr.fence);
		dlg_assert(res == VK_SUCCESS);

		finishLocked(dev, cr);
	}

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

bool aliasesOtherResourceLocked(const MemoryResource& res) {
	dlg_assert_or(res.memory, return false);
	auto& mem = *res.memory;
	assertOwned(res.dev->mutex);

	auto it = std::lower_bound(mem.allocations.begin(), mem.allocations.end(),
		res, cmpMemRes);
	dlg_assert(it != mem.allocations.end());
	if(*it != &res) {
		dlg_assert((*it)->allocationOffset == res.allocationOffset);
		return true;
	}

	++it;
	auto imgEnd = res.allocationOffset + res.allocationSize;
	if(it != mem.allocations.end() && (*it)->allocationOffset < imgEnd) {
		return true;
	}

	return false;
}

bool currentlyMappedLocked(const MemoryResource& res) {
	dlg_assert_or(res.memory, return false);
	auto& mem = *res.memory;
	assertOwned(res.dev->mutex);

	return (mem.map && overlaps(res.allocationOffset, res.allocationSize,
				mem.mapOffset, mem.mapSize));
}

} // namespace vil
