#pragma once

// TODO: leave to build system/config file
#define VK_USE_PLATFORM_WAYLAND_KHR

#include "fwd.hpp"

#include <vkpp/dispatch.hpp>
#include <vkpp/span.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdint>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <thread>
#include <deque>
#include <atomic>

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

// util
u32 findLSB(u32 v);

template<typename C>
void ensureSize(C& container, std::size_t size) {
	if(container.size() < size) {
		container.resize(size);
	}
}

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;
	virtual void update() = 0;
};

struct Instance {
	vk::DynamicDispatch dispatch;
	VkInstance ini;

	struct {
		u32 version;
		u32 apiVersion;
		u32 engineVersion;
		std::string name;
		std::string engineName;
	} app;
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

	VkDescriptorSet dsSelected;

	void init(Device& dev);
};

struct Renderer {
	Device* dev;
	VkRenderPass rp;
	VkPipeline pipe;

	bool clear {};
	ImGuiContext* imgui;

	VkDescriptorSet dsFont;

	struct {
		// image
		Image* image {};
		VkImageView view {};
		VkImageAspectFlagBits aspectMask {};

		// command buffer
		CommandBuffer* cb {};
	} selected;

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

	void drawImagesGui(Draw&);
	void drawOverviewGui(Draw&);
	void drawBuffersGui(Draw&);
	void drawCbsGui();

	void drawGui(Draw&);
	void uploadDraw(Draw&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb, bool force);
};

// When rendering directly onto the hooked window
struct Overlay {
	Swapchain* swapchain;
	Renderer renderer;
	std::vector<RenderBuffer> buffers;
	std::vector<Draw> draws;
	Platform* platform;

	void init(Swapchain& swapchain);
	VkResult drawPresent(Queue& queue, span<const VkSemaphore>, u32 imageIdx);
};

struct Swapchain {
	Device* dev;
	VkSwapchainKHR swapchain;
	VkSwapchainCreateInfoKHR ci;
	std::string name;

	bool useOverlay {};
	Overlay overlay;

	std::vector<Image*> images;

	~Swapchain() {
		// TODO
	}
};

struct Queue {
	Device* dev;
	std::string name;

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

// Synchronized unordered map.
// Elements are stored in unique_ptr's, making sure that as long as two
// threads never operate on the same entry and lookup/creation/destruction
// of entries is synchronized, everything just works.
// You should never (outside of a lock and we try to limit the locks to
// the immediate lookup/creation/destruction sections) work with iterators
// or unique_ptr<Value> refs of this map since they might get destroyed
// at *any* moment, when the unordered map needs a rehash.
template<typename K, typename T>
class SyncedUniqueUnorderedMap {
public:
	using UnorderedMap = std::unordered_map<K, std::unique_ptr<T>>;

	std::unique_ptr<T> move(const K& key) {
		std::lock_guard lock(*mutex);
		auto it = map.find(key);
		if(it == map.end()) {
			return nullptr;
		}

		auto ret = std::move(it->second);
		map.erase(it);
		return ret;
	}

	std::unique_ptr<T> mustMove(const K& key) {
		auto ret = move(key);
		assert(ret);
		return ret;
	}

	std::size_t erase(const K& key) {
		std::lock_guard lock(*mutex);
		return map.erase(key);
	}

	std::size_t mustErase(const K& key) {
		std::lock_guard lock(*mutex);
		auto count = map.erase(key);
		assert(count);
		return count;
	}

	T* find(const K& key) {
		std::shared_lock lock(*mutex);
		auto it = map.find(key);
		return it == map.end() ? nullptr : it->second.get();
	}

	// Expects an element in the map, finds and returns it.
	// Unlike operator[], will never create the element.
	// Error to call this with a key that isn't present.
	T& get(const K& key) {
		std::shared_lock lock(*mutex);
		auto it = map.find(key);
		assert(it != map.end());
		return *it->second.get();
	}

	template<class O>
	bool contains(const O& x) const {
		// TODO C++20, use map.contains
		std::shared_lock lock(*mutex);
		return map.find(x) != map.end();
	}

	T& operator[](const K& key) {
		std::lock_guard lock(*mutex);
		return map[key];
	}

	// emplace methods may be counter-intuitive.
	// You must actually pass a unique_ptr<T> as value.
	// Might wanna use add() instead.
	template<class... Args>
	std::pair<T*, bool> emplace(Args&&... args) {
		std::lock_guard lock(*mutex);
		auto [it, success] = map.emplace(std::forward<Args>(args)...);
		return {it->second.get(), success};
	}

	template<class... Args>
	T& mustEmplace(Args&&... args) {
		auto [ptr, success] = this->emplace(std::forward<Args>(args)...);
		assert(success);
		return *ptr;
	}

	// Asserts that element is really new
	template<typename V = T, class... Args>
	T& add(const K& key, Args&&... args) {
		auto elem = std::make_unique<V>(std::forward<Args>(args)...);
		return this->mustEmplace(key, std::move(elem));
	}

	// Keep in mind they can immediately be out-of-date.
	bool empty() const {
		std::shared_lock lock(*mutex);
		return map.empty();
	}

	std::size_t size() const {
		std::shared_lock lock(*mutex);
		return map.size();
	}

	// Can also be used directly, but take care!
	std::shared_mutex* mutex;
	UnorderedMap map;
};

struct Submission {
	std::vector<std::pair<VkSemaphore, VkPipelineStageFlags>> waitSemaphores;
	std::vector<VkSemaphore> signalSemaphore;

	// Accessing those might not be save if the submission has completed,
	// they might have already been destroyed. So:
	// - lock dev.mutex (shared lock is enough)
	// - check whether associated pending fence has completed
	// - only if not are you allowed to acces them (and only while
	//   mutex is still locked)
	std::vector<CommandBuffer*> cbs;
};

struct PendingSubmission {
	Queue* queue {};
	std::vector<Submission> submissions;

	// The fence added by the caller.
	// Might be null
	Fence* appFence {};

	// When the caller didn't add a fence, we added this one from the fence pool.
	// When appFence is not null, this is null.
	VkFence ourFence {};
	std::mutex ourMutex;
};

// Expects dev.mutex to be locked
bool checkLocked(PendingSubmission& subm);

struct Device {
	Instance* ini;
	VkDevice dev;
	VkPhysicalDevice phdev;
	vk::DynamicDispatch dispatch;

	PFN_vkSetDeviceLoaderData setDeviceLoaderData;

	std::vector<Queue> queues;
	Queue* gfxQueue;

	VkDescriptorPool dsPool;
	VkSampler sampler;
	VkDescriptorSetLayout dsLayout;
	VkPipelineLayout pipeLayout;
	VkCommandPool commandPool;

	u32 hostVisibleMemTypeBits;
	u32 deviceLocalMemTypeBits;

	DisplayWindow window;

	std::vector<VkFence> fencePool; // currently unused fences
	std::vector<std::unique_ptr<PendingSubmission>> pending;

	// Mutex for general shared access.
	// While this mutex is locked, resources won't be inserted or
	// erased from the resource tables below (and therefore can't
	// logically be created or destroyed).
	std::shared_mutex mutex;
	std::mutex queueMutex; // mutex for accessing queues

	SyncedUniqueUnorderedMap<VkSwapchainKHR, Swapchain> swapchains;

	SyncedUniqueUnorderedMap<VkImage, Image> images;
	SyncedUniqueUnorderedMap<VkImageView, ImageView> imageViews;
	SyncedUniqueUnorderedMap<VkSampler, Sampler> samplers;
	SyncedUniqueUnorderedMap<VkBuffer, Buffer> buffers;
	SyncedUniqueUnorderedMap<VkPipelineLayout, PipelineLayout> pipeLayouts;
	SyncedUniqueUnorderedMap<VkPipeline, ComputePipeline> computePipes;
	SyncedUniqueUnorderedMap<VkPipeline, GraphicsPipeline> graphicsPipes;
	SyncedUniqueUnorderedMap<VkFramebuffer, Framebuffer> framebuffers;
	SyncedUniqueUnorderedMap<VkRenderPass, RenderPass> renderPasses;
	SyncedUniqueUnorderedMap<VkCommandPool, CommandPool> commandPools;
	SyncedUniqueUnorderedMap<VkCommandBuffer, CommandBuffer> commandBuffers;
	SyncedUniqueUnorderedMap<VkFence, Fence> fences;
	SyncedUniqueUnorderedMap<VkDescriptorPool, DescriptorPool> dsPools;
	SyncedUniqueUnorderedMap<VkDescriptorSetLayout, DescriptorSetLayout> dsLayouts;
	SyncedUniqueUnorderedMap<VkDescriptorSet, DescriptorSet> descriptorSets;
	SyncedUniqueUnorderedMap<VkShaderModule, ShaderModule> shaderModules;
	SyncedUniqueUnorderedMap<VkDeviceMemory, DeviceMemory> deviceMemories;

	// NOTE: when adding new maps: also add mutex initializer in CreateDevice
};

template<typename F>
void forEachRenderer(Device& dev, F&& func) {
	std::lock_guard guard(dev.mutex);
	for(auto& swapchain : dev.swapchains.map) {
		if(!swapchain.second->useOverlay) {
			continue;
		}

		func(swapchain.second->overlay.renderer);
	}

	if(dev.window.display) {
		func(dev.window.renderer);
	}
}

} // namespace fuen
