#pragma once

#include "device.hpp"
#include "renderer.hpp"
#include <variant>

struct ImGuiContext;
struct ImGuiIO;

namespace fuen {

class Gui {
public:
	enum class Tab {
		overview,
		resources,
		commandBuffer,

		// TODO
		// image,
		// buffer
	};

public:
	void init(Device& dev);
	~Gui();

	void draw(Draw&, bool fullscreen);
	void makeImGuiCurrent();

	void unselect(const Handle& handle);
	void activateTab(Tab);

	template<typename T>
	void select(T& handle, bool activateTab = true) {
		selected_.handle = handle;
		if(activateTab) {
			this->activateTab(Tab::overview);
		}
	}

	auto& selectedHandle() const { return selected_.handle; }

	VkImage selectedImage() const { return selected_.image.handle; }
	bool selectedImageViewable() const { return (selected_.image.view); }
	VkImageAspectFlagBits selectedImageAspect() const { return selected_.image.aspectMask; }
	ImGuiIO& imguiIO() const { return *io_; }

private:
	void drawMemoryResourceUI(Draw&, MemoryResource&);
	void drawResourceUI(Draw&, Image&);
	void drawResourceUI(Draw&, ImageView&);
	void drawResourceUI(Draw&, Framebuffer&);
	void drawResourceUI(Draw&, RenderPass&);
	void drawResourceUI(Draw&, Buffer&);
	void drawResourceUI(Draw&, Sampler&);
	void drawResourceUI(Draw&, DescriptorSet&);
	void drawResourceUI(Draw&, DescriptorPool&);
	void drawResourceUI(Draw&, DescriptorSetLayout&);
	void drawResourceUI(Draw&, GraphicsPipeline&);
	void drawResourceUI(Draw&, ComputePipeline&);
	void drawResourceUI(Draw&, PipelineLayout&);
	void drawResourceUI(Draw&, DeviceMemory&);
	void drawResourceUI(Draw&, CommandPool&);
	void drawResourceUI(Draw&, CommandBuffer&);
	void drawResourceUI(Draw&, ShaderModule&);

	void drawCommandBufferInspector(Draw&, CommandBuffer& cb);

	void drawResourceSelectorUI(Draw&);
	void drawOverviewUI(Draw&);

private:
	Device* dev_ {};
	ImGuiContext* imgui_ {};
	ImGuiIO* io_ {};

	std::string search_;
	int filter_ {0};

	using HandleVariant = std::variant<
		std::monostate, // empty
		Image*,
		ImageView*,
		Sampler*,
		Framebuffer*,
		RenderPass*,
		Buffer*,
		DeviceMemory*,
		CommandBuffer*,
		CommandPool*,
		DescriptorPool*,
		DescriptorSet*,
		DescriptorSetLayout*,
		GraphicsPipeline*,
		ComputePipeline*,
		PipelineLayout*,
		ShaderModule*>;

	struct {
		HandleVariant handle;

		// Image data
		struct {
			Image* object {};
			VkImageView view {};
			VkImageAspectFlagBits aspectMask {};
		} image;

		// For command buffer selector tab.
		struct {
			CommandBuffer* cb {}; // the selected command buffer
			const Command* command {}; // the selected command inside the cb
			u32 resetCount {}; // the resetCount of cb at which teh command was valid
		} cb;

		// Data for the buffer viewer
		struct {
			VkBuffer handle {};
		} buffer;

		// These two can be used to activate a tab:
		// Set tab to the tab id to be actived and tabCounter to zero.
		// Due the way ImGui is designed we need some hackery, activating
		// the tab for multiple frames, that's what counter is for.
		Tab tab {};
		u32 tabCounter {10};
	} selected_;

};

/*
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

		// Will ensure the buffer has at least the given size.
		// If not, will recreate it with the given size and usage.
		// Always uses host visible memory.
		void ensure(Device& dev, VkDeviceSize, VkBufferUsageFlags);
	};

	Buffer vertexBuffer;
	Buffer indexBuffer;
	Buffer readbackBuffer; // TODO: use!
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
	ImGuiContext* imgui {};

	VkDescriptorSet dsFont;

	struct {
		std::variant<
			// empty
			std::monostate,

			// handles
			Image*,
			ImageView*,
			Sampler*,
			Framebuffer*,
			RenderPass*,
			Buffer*,
			DeviceMemory*,
			CommandBuffer*,
			CommandPool*,
			DescriptorPool*,
			DescriptorSet*,
			DescriptorSetLayout*,
			GraphicsPipeline*,
			ComputePipeline*,
			PipelineLayout*,
			ShaderModule*,

			// custom stuff
			SpirvData*
		> handle;

		// image data
		struct {
			VkImage handle {};
			VkImageView view {};
			VkImageAspectFlagBits aspectMask {};
		} image;

		// For command buffer selector
		struct {
			CommandBuffer* cb {};
			const Command* command {};
			u32 resetCount {}; // for command
		} cb;

		struct {
			std::vector<std::byte> data;
		} buffer;

		// These two can be used to activate a tab:
		// Set tab to the tab id to be actived and tabCounter to zero.
		// Due the way ImGui is designed we need some hackery, activating
		// the tab for multiple frames, that's what counter is for.
		u32 tab {};
		u32 tabCounter {10};
	} selected;

	struct {
		bool uploaded {};
		VkDeviceMemory mem {};
		VkImage image {};
		VkImageView view {};

		VkDeviceMemory uploadMem {};
		VkBuffer uploadBuf {};
	} font;

public:
	Renderer() = default;
	Renderer(Renderer&&) = delete;
	Renderer& operator=(Renderer&&) = delete;
	~Renderer();

	void init(Device& dev, VkFormat, bool clear);
	void ensureFontAtlas(VkCommandBuffer cb);

	void drawMemoryResourceUI(Draw&, MemoryResource&);
	void drawResourceUI(Draw&, Image&);
	void drawResourceUI(Draw&, ImageView&);
	void drawResourceUI(Draw&, Framebuffer&);
	void drawResourceUI(Draw&, RenderPass&);
	void drawResourceUI(Draw&, Buffer&);
	void drawResourceUI(Draw&, Sampler&);
	void drawResourceUI(Draw&, DescriptorSet&);
	void drawResourceUI(Draw&, DescriptorPool&);
	void drawResourceUI(Draw&, DescriptorSetLayout&);
	void drawResourceUI(Draw&, GraphicsPipeline&);
	void drawResourceUI(Draw&, ComputePipeline&);
	void drawResourceUI(Draw&, PipelineLayout&);
	void drawResourceUI(Draw&, DeviceMemory&);
	void drawResourceUI(Draw&, CommandPool&);
	void drawResourceUI(Draw&, CommandBuffer&);
	void drawResourceUI(Draw&, ShaderModule&);

	void drawResourceUI(Draw&, SpirvData&);

	void drawCommandBufferInspector(Draw&, CommandBuffer& cb);

	void drawResourceSelectorUI(Draw&);
	void drawOverviewUI(Draw&);

	void drawGui(Draw&, bool fullscreen);
	void uploadDraw(Draw&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb, bool force);

	// unselects the given handle, if it was selected.
	void unselect(const Handle& handle);
	void unselect(const void* handle);
};

// Can be associated with a VkSurfaceKHR
struct Platform {
	virtual ~Platform() = default;
	virtual void update() = 0;
};

// When rendering directly onto the hooked window
struct Overlay {
	Swapchain* swapchain;
	Renderer renderer;
	std::vector<RenderBuffer> buffers;
	std::vector<Draw> draws;
	Platform* platform;
	bool show {};

	void init(Swapchain& swapchain);
	void initRenderBuffers(); // called from init
	VkResult drawPresent(Queue& queue, span<const VkSemaphore>, u32 imageIdx);
};
*/

} // namespace fuen
