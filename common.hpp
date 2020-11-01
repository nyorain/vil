#pragma once

#define VK_NO_PROTOTYPES

// TODO: leave to build system/config file
#define VK_USE_PLATFORM_WAYLAND_KHR

#include <vkpp/dispatch.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <memory>

struct ImGuiContext;

namespace fuen {

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;
	virtual void update() = 0;
};

using u32 = std::uint32_t;
using i32 = std::int32_t;

struct Device;
struct Instance;

struct Instance {
	vk::DynamicDispatch dispatch;
	VkInstance ini;
};

struct Image {
	Device* dev;
	std::string name;
	VkImageCreateInfo ci;
	VkImageView view;
};

struct Buffer {
	Device* dev;
	std::string name;
	VkBufferCreateInfo ci;
};

// All data used for drawing a single frame of the overlay.
struct Draw {
	struct Buffer {
		VkBuffer buf;
		VkDeviceMemory mem;
		VkDeviceSize size;

		void ensure(Device& dev, VkDeviceSize, VkBufferUsageFlags);
	};

	Buffer vertexBuffer;
	Buffer indexBuffer;
	VkCommandBuffer cb;
	VkSemaphore semaphore;
	VkFence fence;
	VkDescriptorSet ds;
};

struct Swapchain {
	Device* dev;
	Platform* platform;
	VkSwapchainKHR swapchain;
	VkSwapchainCreateInfoKHR ci;

	struct Buffer {
		VkImage image;
		VkImageView view;
		VkFramebuffer fb;
	};

	VkRenderPass rp;
	VkPipeline pipe;

	std::vector<Buffer> buffers;
	std::vector<Draw> draws;

	ImGuiContext* imgui;

	struct {
		bool uploaded;
		VkDeviceMemory mem;
		VkImage image;
		VkImageView view;

		VkDeviceMemory uploadMem;
		VkBuffer uploadBuf;
	} font;

	~Swapchain() {
		// TODO
	}
};

struct Queue {
	Device* dev;

	VkQueue queue;
	VkQueueFlags flags;
	u32 family;
};

struct Device {
	Instance* ini;
	VkDevice dev;
	vk::DynamicDispatch dispatch;

	PFN_vkSetDeviceLoaderData setDeviceLoaderData;

	std::vector<Queue> queues;
	Queue* gfxQueue;

	std::shared_mutex mutex;
	std::unordered_map<VkImage, std::unique_ptr<Image>> images;
	std::unordered_map<VkBuffer, std::unique_ptr<Buffer>> buffers;
	std::unordered_map<VkSwapchainKHR, std::unique_ptr<Swapchain>> swapchains;

	VkDescriptorPool dsPool;
	VkSampler sampler;
	VkDescriptorSetLayout dsLayout;
	VkPipelineLayout pipeLayout;
	VkCommandPool commandPool;

	u32 hostVisibleMemTypeBits;
	u32 deviceLocalMemTypeBits;
};

#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assert(result == VK_SUCCESS); \
	} while(0)


} // namespace fuen
