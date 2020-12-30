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

	void selectResource(Handle& handle, bool activateTab = true);
	void selectCommands(IntrusivePtr<CommandRecord> record,
		bool updateFromGroup, bool activateTab = true);

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

// Inserts an imgui button towards the given handle.
// When clicked, selects the handle in the given gui.
void refButton(Gui& gui, Handle& handle);

// If handle isn't null, adds the button as with refButton.
void refButtonOpt(Gui& gui, Handle* handle);

// Asserts that image isn't null and if so, adds the button as with refButton.
void refButtonExpect(Gui& gui, Handle* handle);

// If the given handle is null, inserts a disabled "<Destroyed>" button.
// Otherwise, normally inserts the button as with refButton.
void refButtonD(Gui& gui, Handle* handle, const char* str = "<Destroyed>");

} // namespace fuen
