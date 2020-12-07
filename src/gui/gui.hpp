#pragma once

#include <device.hpp>
#include <renderer.hpp>
#include <gui/resources.hpp>
#include <variant>

struct ImGuiContext;
struct ImGuiIO;

namespace fuen {

struct BufferGui {
	enum class Type {
		f1, f2, f3, f4,
		d1, d2, d3, d4,
		i1, i2, i3, i4,
		u1, u2, u3, u4,
		mat2, mat3, mat4,
		eBool
	};

	Buffer* buffer_;
	VkDeviceSize offset_ {};
	VkDeviceSize size_ {};
	std::vector<std::byte> lastRead_;
	std::vector<Type> layout_;
};

struct ImageGui {
	void draw();

	Image* image_;
	VkImageSubresourceRange subres_;
	VkImageView view_;
};

struct CommandBufferGui {
	void draw();

	CommandBuffer* cb_ {}; // the selected command buffer
	const Command* command_ {}; // the selected command inside the cb
	u32 resetCount_ {}; // the resetCount of cb at which teh command was valid
};

class Gui {
public:
	enum class Tab {
		overview,
		resources,
		commandBuffer,
		image,
		buffer,
	};

public:
	Gui() = default;
	Gui(Gui&&) = delete;
	Gui& operator=(Gui&&) = delete;
	~Gui();

	void init(Device& dev, VkFormat format, bool clear);

	struct FrameInfo {
		VkSwapchainKHR swapchain {};
		u32 imageIdx {};
		VkFramebuffer fb {};
		bool forceClear {};
		bool fullscreen {};

		std::vector<VkSemaphore> waitSemaphores;
		std::vector<VkPipelineStageFlags> waitStages;
	};

	void renderFrame(Draw&, Gui&, FrameInfo& info);

	void makeImGuiCurrent();

	void unselect(const Handle& handle);
	void activateTab(Tab);

	template<typename T>
	void select(T& handle, bool activateTab = true) {
		tabs_.resources.select(handle);
		if(activateTab) {
			this->activateTab(Tab::overview);
		}
	}

	ImGuiIO& imguiIO() const { return *io_; }

	Device& dev() const { return *dev_; }
	VkRenderPass rp() const { return rp_; }
	VkPipeline pipe() const { return pipe_; }

private:
	void draw(Draw&, bool fullscreen);
	void drawOverviewUI(Draw&);
	void ensureFontAtlas(VkCommandBuffer cb);

	void uploadDraw(Draw&, const ImDrawData&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb,
		bool drawEvenWhenEmpty, const ImDrawData&);

private:
	Device* dev_ {};
	ImGuiContext* imgui_ {};
	ImGuiIO* io_ {};

	struct {
		ResourceGui resources;
		CommandBufferGui cb;
		ImageGui image;
		BufferGui buffer;
	} tabs_;

	// rendering stuff
	VkRenderPass rp_ {};
	VkPipeline pipe_ {};
	VkCommandPool commandPool_ {};

	bool clear_ {};
	VkDescriptorSet dsFont_ {};

	struct {
		bool uploaded {};
		VkDeviceMemory mem {};
		VkImage image {};
		VkImageView view {};

		VkDeviceMemory uploadMem {};
		VkBuffer uploadBuf {};
	} font_;

};

} // namespace fuen
