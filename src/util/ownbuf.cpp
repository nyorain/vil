#include <util/ownbuf.hpp>
#include <util/util.hpp>
#include <device.hpp>
#include <queue.hpp>
#include <threadContext.hpp>

namespace vil {

void OwnBuffer::ensure(Device& dev, VkDeviceSize reqSize,
		VkBufferUsageFlags usage, u32 queueFamsBitset, StringParam name,
		Type type) {
	dlg_assert(!this->dev || this->dev == &dev);
	if(size >= reqSize) {
		return;
	}

	this->dev = &dev;

	if(buf) {
		dev.dispatch.DestroyBuffer(dev.handle, buf, nullptr);
		dev.dispatch.FreeMemory(dev.handle, mem, nullptr);

		DebugStats::get().ownBufferMem -= size;

		mem = {};
		buf = {};
		size = {};
	}

	// new buffer
	VkBufferCreateInfo bufInfo {};
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size = reqSize;
	bufInfo.usage = usage;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	ThreadMemScope ms;
	auto qfs = ms.allocRaw<u32>(32);
	auto qfcount = 0u;

	for(auto i = 0u; i < 32; ++i) {
		if(queueFamsBitset & (1u << i)) {
			qfs[qfcount++] = i;
		}
	}

	if(qfcount >= 2) {
		bufInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
		bufInfo.pQueueFamilyIndices = qfs;
		bufInfo.queueFamilyIndexCount = qfcount;
	}

	VK_CHECK_DEV(dev.dispatch.CreateBuffer(dev.handle, &bufInfo, nullptr, &buf), dev);
	nameHandle(dev, this->buf, name.empty() ? "OwnBuffer:buf" : name.c_str());

	// get memory props
	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, buf, &memReqs);
	memReqs.size = align(memReqs.size, dev.props.limits.nonCoherentAtomSize);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = align(memReqs.size, dev.props.limits.nonCoherentAtomSize);

	auto memBits = (type == Type::hostVisible) ?
		dev.hostVisibleMemTypeBits :
		dev.deviceLocalMemTypeBits;
	dlg_assert(memBits != 0u);
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & memBits);

	VkMemoryAllocateFlagsInfo flagsInfo {};
	if(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		allocInfo.pNext = &flagsInfo;
	}

	VK_CHECK_DEV(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &mem), dev);
	nameHandle(dev, this->mem, "OwnBuffer:mem");

	// bind
	VK_CHECK_DEV(dev.dispatch.BindBufferMemory(dev.handle, buf, mem, 0), dev);
	this->size = reqSize;

	// Might not be 100% accurate for used memory but good enough
	DebugStats::get().ownBufferMem += size;

	// map
	if(type == Type::hostVisible) {
		void* pmap;
		VK_CHECK_DEV(dev.dispatch.MapMemory(dev.handle, mem, 0, VK_WHOLE_SIZE, 0, &pmap), dev);
		this->map = static_cast<std::byte*>(pmap);
		dlg_assert(this->map);
	}
}

OwnBuffer::~OwnBuffer() {
	if(!dev) {
		return;
	}

	// no need to unmap memory, automtically done when memory is destroyed

	dev->dispatch.DestroyBuffer(dev->handle, buf, nullptr);
	dev->dispatch.FreeMemory(dev->handle, mem, nullptr);

	DebugStats::get().ownBufferMem -= size;
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
	VK_CHECK_DEV(dev->dispatch.InvalidateMappedMemoryRanges(dev->handle, 1, range), *dev);
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
	VK_CHECK_DEV(dev->dispatch.FlushMappedMemoryRanges(dev->handle, 1, range), *dev);
}

vku::BufferSpan OwnBuffer::asSpan(VkDeviceSize offset, VkDeviceSize size) const {
	vku::BufferSpan ret;
	ret.buffer = buf;
	ret.allocation.offset = offset;
	ret.allocation.size = size == VK_WHOLE_SIZE ? this->size - offset : size;
	dlg_assert(ret.allocation.offset + ret.allocation.size <= this->size);
	return ret;
}

void swap(OwnBuffer& a, OwnBuffer& b) noexcept {
	using std::swap;
	swap(a.dev, b.dev);
	swap(a.buf, b.buf);
	swap(a.mem, b.mem);
	swap(a.size, b.size);
	swap(a.map, b.map);
}

} // namespace vil
