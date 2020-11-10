#pragma once

// Make sure to *never* include them
#define VK_NO_PROTOTYPES

#include <vkpp/span.hpp>

namespace fuen {

// C++20: replace with std::span
using nytl::span;

struct Device;
struct Instance;
struct Queue;
struct Swapchain;

struct Image;
struct ImageView;
struct Framebuffer;
struct RenderPass;
struct CommandBuffer;

struct Fence;
struct Event;
struct CommandPool;

struct DescriptorPool;
struct DescriptorSet;
struct DescriptorSetLayout;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i16 = std::int16_t;
using i32 = std::int32_t;

} // namespace fuen
