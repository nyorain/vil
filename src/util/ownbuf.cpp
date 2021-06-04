#include <util/ownbuf.hpp>
#include <device.hpp>

namespace vil {

void OwnBuffer::ensure(Device& dev, VkDeviceSize reqSize,
		VkBufferUsageFlags usage) {
	dlg_assert(!this->dev || this->dev == &dev);
	if(size >= reqSize) {
		return;
	}

	this->dev = &dev;

	if(buf) {
		dev.dispatch.DestroyBuffer(dev.handle, buf, nullptr);
		dev.dispatch.FreeMemory(dev.handle, mem, nullptr);
	}

	// new buffer
	VkBufferCreateInfo bufInfo {};
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size = reqSize;
	bufInfo.usage = usage;
	VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bufInfo, nullptr, &buf));
	nameHandle(dev, this->buf, "OwnBuffer:buf");

	// get memory props
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf, &memReqs);
	memReqs.size = align(memReqs.size, dev.props.limits.nonCoherentAtomSize);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = align(memReqs.size, dev.props.limits.nonCoherentAtomSize);
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits);

	VkMemoryAllocateFlagsInfo flagsInfo {};
	if(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		allocInfo.pNext = &flagsInfo;
	}

	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &mem));
	nameHandle(dev, this->mem, "OwnBuffer:mem");

	// bind
	VK_CHECK(dev.dispatch.BindBufferMemory(dev.handle, buf, mem, 0));
	this->size = reqSize;

	// map
	void* pmap;
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, mem, 0, VK_WHOLE_SIZE, 0, &pmap));
	this->map = static_cast<std::byte*>(pmap);
}

OwnBuffer::~OwnBuffer() {
	if(!dev) {
		return;
	}

	// no need to unmap memory, automtically done when memory is destroyed

	dev->dispatch.DestroyBuffer(dev->handle, buf, nullptr);
	dev->dispatch.FreeMemory(dev->handle, mem, nullptr);
}

void OwnBuffer::invalidateMap() {
	if(!mem) {
		dlg_warn("invalidateMap: invalid buffer");
		return;
	}

	// PERF: only invalidate when on non-coherent memory
	VkMappedMemoryRange range[1] {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = mem;
	range[0].size = VK_WHOLE_SIZE;
	VK_CHECK(dev->dispatch.InvalidateMappedMemoryRanges(dev->handle, 1, range));
}

void OwnBuffer::flushMap() {
	if(!mem) {
		dlg_warn("flushMap: invalid buffer");
		return;
	}

	// PERF: only invalidate when on non-coherent memory
	VkMappedMemoryRange range[1] {};
	range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range[0].memory = mem;
	range[0].size = VK_WHOLE_SIZE;
	VK_CHECK(dev->dispatch.FlushMappedMemoryRanges(dev->handle, 1, range));
}

void swap(OwnBuffer& a, OwnBuffer& b) noexcept {
	using std::swap;
	swap(a.dev, b.dev);
	swap(a.buf, b.buf);
	swap(a.mem, b.mem);
	swap(a.size, b.size);
}

} // namespace vil
