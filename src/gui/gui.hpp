#pragma once

#include <device.hpp>
#include <gui/renderer.hpp>
#include <gui/resources.hpp>
#include <gui/cb.hpp>
#include <imgui/imgui.h>
#include <guidraw.hpp>
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
		VkExtent2D extent {};
		VkFramebuffer fb {};
		bool fullscreen {};
		VkQueue presentQueue {};

		span<const VkSemaphore> waitSemaphores;
	};

	struct FrameResult {
		VkResult result;
		Draw* draw;
	};

	FrameResult renderFrame(FrameInfo& info);

	void makeImGuiCurrent();

	// Must only be called while device mutex is locked.
	void destroyed(const Handle& handle);

	// Must only be called when it is guaranteed that no other thread
	// is drawing at the same time (externally synchronized).
	void finishDraws();
	void activateTab(Tab);

	template<typename T>
	void selectResource(T& handle, bool activateTab = true) {
		tabs_.resources.select(handle);
		if(activateTab) {
			this->activateTab(Tab::resources);
		}
	}

	void selectCb(CommandBuffer& cb, bool activateTab = true) {
		// TODO
		(void) cb;
		(void) activateTab;
		/*
		tabs_.cb.select(cb);
		if(activateTab) {
			this->activateTab(Tab::commandBuffer);
		}
		*/
	}

	ImGuiIO& imguiIO() const { return *io_; }

	Device& dev() const { return *dev_; }
	VkRenderPass rp() const { return rp_; }

private:
	void draw(Draw&, bool fullscreen);
	void drawOverviewUI(Draw&);
	void ensureFontAtlas(VkCommandBuffer cb);

	void uploadDraw(Draw&, const ImDrawData&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb, const ImDrawData&);

	// Functions expect device mutex to be locked
	template<typename H>
	void waitForSubmissions(const H& handle);
	void waitFor(span<PendingSubmission*> submsisions);

private:
	Device* dev_ {};
	ImGuiContext* imgui_ {};
	ImGuiIO* io_ {};

	Tab activeTab_ {};
	u32 activateTabCounter_ {};

	std::vector<Draw> draws_;

	struct {
		ResourceGui resources;
		CommandBufferGui cb;
	} tabs_;

	// rendering stuff
	VkRenderPass rp_ {};
	VkCommandPool commandPool_ {};

	struct {
		VkPipeline gui;
		VkPipeline image1D;
		VkPipeline image2D;
		VkPipeline image3D;
	} pipes_;

	bool clear_ {};
	VkDescriptorSet dsFont_ {};

	struct {
		bool uploaded {};
		VkDeviceMemory mem {};
		VkImage image {};
		VkImageView view {};

		VkDeviceMemory uploadMem {};
		VkBuffer uploadBuf {};

		DrawGuiImage drawImage {};
	} font_;

	Draw::Buffer readbackBuf_;

	using Clock = std::chrono::high_resolution_clock;
	Clock::time_point lastFrame_ {};
};

template<typename T>
void resourceRefButton(Gui& gui, T& resource) {
	if(ImGui::Button(name(resource).c_str())) {
		gui.selectResource(resource);
	}
}

} // namespace fuen
