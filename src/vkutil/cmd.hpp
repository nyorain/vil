#pragma once

#include <fwd.hpp>

namespace vil::vku {

void cmdCopyBuffer(Device& dev, VkCommandBuffer, BufferSpan src, BufferSpan dst);

} // namespace vil::vku
