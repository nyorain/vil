#pragma once

#include <gui/render.hpp>
#include <gui/blur.hpp>
#include <util/bytes.hpp>
#include <util/vec.hpp>
#include <util/util.hpp>
#include <variant>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <chrono>

namespace vil {

class ResourceGui;
class CommandRecordGui;

class Gui {
public:
	enum class Tab {
		overview,
		resources,
		commandBuffer,
		memory,
	};

	struct Event {
		enum class Type {
			key,
			mouseButton,
			mousePos,
			mouseWheel,
			input,
			input16, // using wstring
		};

		Type type;
		ImGuiKey key;
		int button;
		bool b; // focused/down
		Vec2f vec2f; // wheel or position
		std::string input;
		unsigned short input16;
	};

	struct FrameInfo {
		VkSwapchainKHR swapchain {};
		u32 imageIdx {};
		VkExtent2D extent {};
		VkFramebuffer fb {};
		VkImage image {};
		bool fullscreen {};
		bool clear {};
		VkQueue presentQueue {};

		span<const VkSemaphore> waitSemaphores;
	};

	struct Pipelines {
		VkPipeline gui {};
		VkPipeline imageBg {};
		std::array<VkPipeline, ShaderImageType::count> image {};

		std::array<VkPipeline, ShaderImageType::count> readTex {};
		std::array<VkPipeline, ShaderImageType::count> minMaxTex {};
		std::array<VkPipeline, ShaderImageType::count> histogramTex {};

		VkPipeline histogramPrepare {};
		VkPipeline histogramMax {};
		VkPipeline histogramRender {};
	};

	bool unfocus {false};

	ImFont* defaultFont {};
	ImFont* monoFont {};

	// TODO: make this into a setting
	static constexpr bool showHelp = true;

public:
	Gui(Device& dev, VkFormat colorFormat);
	Gui(Gui&&) = delete;
	Gui& operator=(Gui&&) = delete;
	~Gui();

	void makeImGuiCurrent();
	VkResult renderFrame(FrameInfo& info);

	// Informs the Gui that the api handle of the given object
	// has is about to be destroyed (note that this has no implications
	// for the lifetime of the object on our side due to the shared
	// ownership of handle objects).
	// The respective handle was already unset.
	// Must only be called while device mutex is locked.
	void apiHandleDestroyed(const Handle& handle, VkObjectType type);

	// Blocks until all pending draws have finished execution.
	// Does not modify any internal state.
	// Must only be called when it is guaranteed that no other thread
	// is drawing at the same time (externally synchronized).
	void waitForDraws();

	// Returns the latest pending Draw that needs to be synchronized
	// with the given submission batch. In case there is no such Draw,
	// returns nullptr.
	Draw* latestPendingDrawSyncLocked(SubmissionBatch&);

	void updateColorFormat(VkFormat newColorFormat);

	Tab activeTab() const { return activeTab_; }

	void activateTab(Tab);
	void selectResource(Handle& handle, VkObjectType objectType,
		bool activateTab = true);

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

	VkFormat colorFormat() const { return colorFormat_; }
	VkFormat depthFormat() const { return depthFormat_; }

	bool visible() const { return visible_; }
	void visible(bool newVisible);

	// only for the current draw
	using Recorder = std::function<void(Draw&)>;
	void addPreRender(Recorder);
	void addPostRender(Recorder);

	// internally synced event queue
	// can be called from any thread at any time
	// will be forwarded to imgui before rendering
	bool addKeyEvent(ImGuiKey key, bool down);
	void addMousePosEvent(Vec2f pos);
	bool addMouseButtonEvent(int button, bool down);
	bool addMouseWheelEvent(Vec2f dir);
	bool addInputEvent(std::string input);
	bool addInputEvent(unsigned short input16);

private:
	void destroyRenderStuff();
	void initRenderStuff();
	void initImGui();

	// returns VK_INCOMPLETE when something was invalidated mid-draw
	// and tryRender needs to be called again
	VkResult tryRender(Draw&, FrameInfo& info);
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

	VkFormat depthFormat_ {};
	VkFormat colorFormat_ {};

	Tab activeTab_ {};
	u32 activateTabCounter_ {};

	std::deque<Draw> draws_;
	Draw* lastDraw_ {};

	// synced via device mutex
	Draw* currDraw_ {};
	std::atomic<u32> currDrawInvalidated_ {};
	std::condition_variable_any currDrawWait_ {};

	struct {
		std::unique_ptr<ResourceGui> resources;
		std::unique_ptr<CommandRecordGui> cb;
	} tabs_;

	// rendering stuff
	// TODO: some of this is very specific or not only gui-related (e.g.
	//   used by CommandHook). Should find a better place for it.
	//   Just putting it in Device seems bad as well, we don't want
	//   to create all of these pipes if no gui is created.
	//   And some stuff might have gui-specific details.
	//   Maybe split it up, have some pipes here and some in Device?
	//   But then factor out some utilities for easy pipeline creation.
	VkDescriptorSetLayout dsLayout_ {};
	VkPipelineLayout pipeLayout_ {};

	VkDescriptorSetLayout imgOpDsLayout_ {};
	VkPipelineLayout imgOpPipeLayout_ {};

	VkDescriptorSetLayout histogramDsLayout_ {};
	VkPipelineLayout histogramPipeLayout_ {};

	VkRenderPass rp_ {};
	VkCommandPool commandPool_ {};

	Pipelines pipes_;
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
	VkDescriptorSet blurDs_ {};

	std::vector<VkSemaphore> waitSemaphores_;
	std::vector<VkPipelineStageFlags> waitStages_;
	std::vector<VkSemaphore> signalSemaphores_;
	std::vector<u64> waitValues_;
	std::vector<u64> signalValues_;
	VkTimelineSemaphoreSubmitInfo tsInfo_ {};

	std::vector<Recorder> preRender_ {};
	std::vector<Recorder> postRender_ {};

	bool visible_ {false};
	bool showImguiDemo_ {false};

	std::mutex eventMutex_;
	std::vector<Event> events_;
	bool eventCaptureMouse_ {};
	bool eventCaptureKeyboard_ {};
	bool eventWantTextInput_ {};
};

ImGuiKey keyToImGui(unsigned key);

void pushDisabled(bool disabled = true);
void popDisabled(bool disabled = true);

// Inserts an imgui button towards the given handle.
// When clicked, selects the handle in the given gui.
void refButton(Gui& gui, Handle& handle, VkObjectType objectType);

template<typename H>
void refButton(Gui& gui, H& handle) {
	refButton(gui, handle, handle.objectType);
}

// If handle isn't null, adds the button as with refButton.
template<typename H>
void refButtonOpt(Gui& gui, H* handle) {
	if(handle) {
		refButton(gui, *handle);
	}
}

// Asserts that image isn't null and if so, adds the button as with refButton.
template<typename H>
void refButtonExpect(Gui& gui, H* handle) {
	dlg_assert_or(handle, return);
	refButton(gui, *handle);
}

// If the given handle is null, inserts a disabled "<Destroyed>" button.
// Otherwise, normally inserts the button as with refButton.
template<typename H>
void refButtonD(Gui& gui, H* handle, const char* str = "<Destroyed>") {
	if(handle) {
		refButton(gui, *handle);
	} else {
		pushDisabled();
		// NOTE: could add popup to button further explaining what's going on
		ImGui::Button(str);
		popDisabled();
	}
}


} // namespace vil
