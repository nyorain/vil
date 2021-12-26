#pragma once

#include <gui/render.hpp>
#include <gui/blur.hpp>
#include <util/bytes.hpp>
#include <util/vec.hpp>
#include <variant>
#include <deque>
#include <chrono>

namespace vil {

class ResourceGui;
class CommandBufferGui;

class Gui {
public:
	enum class Tab {
		overview,
		resources,
		commandBuffer,
		memory,
	};

	struct FrameInfo {
		VkSwapchainKHR swapchain {};
		u32 imageIdx {};
		VkExtent2D extent {};
		VkFramebuffer fb {};
		bool fullscreen {};
		VkQueue presentQueue {};

		span<const VkSemaphore> waitSemaphores;
	};

	bool visible {false};
	bool unfocus {false};

	ImFont* defaultFont {};
	ImFont* monoFont {};

	// TODO: make this into a setting
	static constexpr bool showHelp = true;

public:
	Gui();
	Gui(Gui&&) = delete;
	Gui& operator=(Gui&&) = delete;
	~Gui();

	// TODO: use constructor instead of init function
	void init(Device& dev, VkFormat colorFormat, VkFormat depthFormat, bool clear);
	void makeImGuiCurrent();
	VkResult renderFrame(FrameInfo& info);

	// Must only be called while device mutex is locked.
	void destroyed(const Handle& handle, VkObjectType type);

	// Blocks until all pending draws have finished execution.
	// Does not modify any internal state.
	// Must only be called when it is guaranteed that no other thread
	// is drawing at the same time (externally synchronized).
	void waitForDraws();

	// Returns the latest pending Draw that needs to be synchronized
	// with the given submission batch. In case there is no such Draw,
	// returns nullptr.
	Draw* latestPendingDrawSyncLocked(SubmissionBatch&);

	Tab activeTab() const { return activeTab_; }

	void activateTab(Tab);
	void selectResource(Handle& handle, bool activateTab = true);

	auto& cbGui() { return *tabs_.cb; }
	ImGuiIO& imguiIO() const { return *io_; }

	Device& dev() const { return *dev_; }
	VkRenderPass rp() const { return rp_; }
	float dt() const { return dt_; }

	Vec2f windowSize() const { return {windowSize_.x, windowSize_.y}; }
	Vec2f windowPos() const { return {windowPos_.x, windowPos_.y}; }

	Queue& usedQueue() const;

	const VkPipeline& imageBgPipe() const { return pipes_.imageBg; }
	const VkPipelineLayout& pipeLayout() const { return pipeLayout_; }
	const VkDescriptorSetLayout& dsLayout() const { return dsLayout_; }

	const VkDescriptorSetLayout& imgOpDsLayout() const { return imgOpDsLayout_; }
	const VkPipelineLayout& imgOpPipeLayout() const { return imgOpPipeLayout_; }
	const VkPipeline& readTexPipe(ShaderImageType::Value type) const { return pipes_.readTex[type]; }

	// only for the current draw
	using Recorder = std::function<void(Draw&)>;
	void addPreRender(Recorder);
	void addPostRender(Recorder);

private:
	void initPipes();
	void initImGui();

	void draw(Draw&, bool fullscreen);
	void drawOverviewUI(Draw&);
	void drawMemoryUI(Draw&);
	void ensureFontAtlas(VkCommandBuffer cb);

	void uploadDraw(Draw&, const ImDrawData&);
	void recordDraw(Draw&, VkExtent2D extent, VkFramebuffer fb, const ImDrawData&);
	void finishedLocked(Draw&);

	[[nodiscard]] VkResult addLegacySync(Draw&, VkSubmitInfo&);
	void addFullSync(Draw&, VkSubmitInfo&);

private:
	Device* dev_ {};
	ImGuiContext* imgui_ {};
	ImGuiIO* io_ {};

	Tab activeTab_ {};
	u32 activateTabCounter_ {};

	std::deque<Draw> draws_;
	Draw* lastDraw_ {};

	struct {
		std::unique_ptr<ResourceGui> resources;
		std::unique_ptr<CommandBufferGui> cb;
	} tabs_;

	// rendering stuff
	VkDescriptorSetLayout dsLayout_ {};
	VkPipelineLayout pipeLayout_ {};

	VkDescriptorSetLayout imgOpDsLayout_ {};
	VkPipelineLayout imgOpPipeLayout_ {};

	VkRenderPass rp_ {};
	VkCommandPool commandPool_ {};

	struct {
		VkPipeline gui;
		VkPipeline imageBg;
		VkPipeline image[ShaderImageType::count] {};
		VkPipeline readTex[ShaderImageType::count] {};
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

	using Clock = std::chrono::high_resolution_clock;
	Clock::time_point lastFrame_ {};

	float dt_ {};
	u64 drawCounter_ {};

	GuiBlur blur_ {};
	VkSwapchainKHR blurSwapchain_ {};
	ImVec2 windowPos_ {};
	ImVec2 windowSize_ {};
	VkDescriptorSet blurDs_;

	// only using during submission might this allows us to split
	// sync building into separate functions
	std::vector<VkSemaphore> waitSemaphores_;
	std::vector<VkPipelineStageFlags> waitStages_;
	std::vector<VkSemaphore> signalSemaphores_;
	std::vector<u64> waitValues_;
	std::vector<u64> signalValues_;
	VkTimelineSemaphoreSubmitInfo tsInfo_ {};

	std::vector<Recorder> preRender_ {};
	std::vector<Recorder> postRender_ {};
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

} // namespace vil
