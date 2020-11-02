#pragma once

#define VK_NO_PROTOTYPES

// TODO: leave to build system/config file
#define VK_USE_PLATFORM_WAYLAND_KHR

#include <vkpp/dispatch.hpp>
#include <vkpp/span.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <thread>

struct ImGuiContext;
struct swa_display;
struct swa_window;

#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assert(result == VK_SUCCESS); \
	} while(0)

// yep, this is bad.
namespace std {
	using nytl::span;
} // namespace std

namespace fuen {

struct Device;
struct Instance;
struct Queue;
struct Swapchain;

using u32 = std::uint32_t;
using i32 = std::int32_t;

// util
u32 findLSB(u32 v);

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;
	virtual void update() = 0;
};

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

struct RenderBuffer {
	Device* dev {};
	VkImage image {};
	VkImageView view {};
	VkFramebuffer fb {};

	void init(Device& dev, VkImage img, VkFormat format, VkExtent2D extent, VkRenderPass rp);
	~RenderBuffer();
};

// All data used for drawing a single frame of the overlay.
struct Draw {
	struct Buffer {
		VkBuffer buf {};
		VkDeviceMemory mem {};
		VkDeviceSize size {};

		void ensure(Device& dev, VkDeviceSize, VkBufferUsageFlags);
	};

	Buffer vertexBuffer;
	Buffer indexBuffer;
	VkCommandBuffer cb;
	VkSemaphore semaphore;
	VkFence fence;
	VkDescriptorSet ds;

	void init(Device& dev);
};

struct Renderer {
	Device* dev;
	VkRenderPass rp;
	VkPipeline pipe;

	bool clear {};
	ImGuiContext* imgui;

	struct {
		bool uploaded {};
		VkDeviceMemory mem {};
		VkImage image {};
		VkImageView view {};

		VkDeviceMemory uploadMem {};
		VkBuffer uploadBuf {};
	} font;

	void init(Device& dev, VkFormat, bool clear);
	void ensureFontAtlas(VkCommandBuffer cb);

	void drawGui();
	void uploadDraw(Draw&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb);
};

// When rendering directly onto the hooked window
struct Overlay {
	Swapchain* swapchain;
	Renderer renderer;
	std::vector<RenderBuffer> buffers;
	std::vector<Draw> draws;
	Platform* platform;

	void init(Swapchain& swapchain);
	VkResult drawPresent(Queue& queue, std::span<const VkSemaphore>, u32 imageIdx);
};

struct Swapchain {
	Device* dev;
	VkSwapchainKHR swapchain;
	VkSwapchainCreateInfoKHR ci;

	bool useOverlay {};
	Overlay overlay;

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

// swa display
struct DisplayWindow {
	swa_display* display;
	swa_window* window;
	Device* dev;
	Renderer renderer;

	VkSurfaceKHR surface {};
	VkSwapchainKHR swapchain {};
	VkSwapchainCreateInfoKHR swapchainCreateInfo {};
	Draw draw {};

	VkSemaphore acquireSem {};
	VkSemaphore renderSem {};

	std::vector<RenderBuffer> buffers;
	std::thread thread;
	bool run {true};

	bool init(Device& dev);
	void resize(unsigned w, unsigned h);
	void initBuffers();
	void destroyBuffers();
	void mainLoop();
};

struct Device {
	Instance* ini;
	VkDevice dev;
	VkPhysicalDevice phdev;
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

	DisplayWindow window;
};

} // namespace fuen
