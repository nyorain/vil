#include <vkutil/cmd.hpp>
#include <vkutil/bufferSpan.hpp>
#include <device.hpp>

namespace vil::vku {

void cmdCopyBuffer(Device& dev, VkCommandBuffer cb, BufferSpan src,
		BufferSpan dst) {
	dlg_assert(src.size() <= dst.size());

	VkBufferCopy copy;
	copy.srcOffset = src.offset();
	copy.dstOffset = dst.offset();
	copy.size = src.size();
	dev.dispatch.CmdCopyBuffer(cb, src.buffer, dst.buffer, 1u, &copy);
}

} // namespace vil::vku
