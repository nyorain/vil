#pragma once

#include <fwd.hpp>
#include <vk/vulkan.h>

namespace vil::vku {

void cmdCopyBuffer(Device& dev, VkCommandBuffer, BufferSpan src, BufferSpan dst);

} // namespace vil::vku
