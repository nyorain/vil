#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <gui/bufferViewer.hpp>
#include <gui/imageViewer.hpp>
#include <ds.hpp>
#include <vk/vulkan.h>
#include <imgui/textedit.h>
#include <variant>
#include <vector>
#include <unordered_set>
#include <string>

namespace vil {

class ResourceGui {
public:
	bool firstUpdate_ {true};

public:
	~ResourceGui();
	void init(Gui& gui);

	void select(Handle& handle, VkObjectType type);
	void draw(Draw&);

private:
	// Draws an ImGui overview for a resource.
	void drawMemoryResDesc(Draw&, MemoryResource&);
	void drawDesc(Draw&, Image&);
	void drawDesc(Draw&, ImageView&);
	void drawDesc(Draw&, Framebuffer&);
	void drawDesc(Draw&, RenderPass&);
	void drawDesc(Draw&, Buffer&);
	void drawDesc(Draw&, Sampler&);
	void drawDesc(Draw&, DescriptorSet&);
	void drawDesc(Draw&, DescriptorPool&);
	void drawDesc(Draw&, DescriptorSetLayout&);
	void drawDesc(Draw&, Pipeline&);
	void drawDesc(Draw&, GraphicsPipeline&);
	void drawDesc(Draw&, ComputePipeline&);
	void drawDesc(Draw&, PipelineLayout&);
	void drawDesc(Draw&, DeviceMemory&);
	void drawDesc(Draw&, CommandPool&);
	void drawDesc(Draw&, CommandBuffer&);
	void drawDesc(Draw&, ShaderModule&);
	void drawDesc(Draw&, Event&);
	void drawDesc(Draw&, Semaphore&);
	void drawDesc(Draw&, Fence&);
	void drawDesc(Draw&, BufferView&);
	void drawDesc(Draw&, QueryPool&);
	void drawDesc(Draw&, Queue&);
	void drawDesc(Draw&, Swapchain&);
	void drawDesc(Draw&, AccelStruct&);
	void drawDesc(Draw&, DescriptorUpdateTemplate&);

	void drawShaderInfo(VkPipeline, VkShaderStageFlagBits stage);
	void drawImageContents(Draw&, Image&, bool doSelect);
	void showBufferViewer(Draw&, Buffer&);

	void drawHandleDesc(Draw&);
	void copyBuffer(Draw&);
	void clearHandles();
	// Will also apply newFilter
	void updateResourceList();
	void clearSelection();

private:
	Gui* gui_ {};
	std::string search_;

	VkObjectType filter_ {VK_OBJECT_TYPE_IMAGE};
	VkObjectType newFilter_ {VK_OBJECT_TYPE_IMAGE};

	// The list of currently displayed handles
	std::vector<Handle*> handles_;

	Handle* handle_ {};
	bool editName_ {false};

	struct {
		Image* object {};
		ImageViewer viewer {};
	} image_;

	struct BufReadback {
		OwnBuffer own;
		VkBuffer src {};
		VkDeviceSize offset {};
		VkDeviceSize size {};
		Draw* pending {};
	};

	struct {
		Buffer* handle {};
		VkDeviceSize offset {};
		VkDeviceSize size {};

		std::vector<BufReadback> readbacks;
		std::optional<unsigned> lastReadback {};
		BufferViewer viewer;
	} buffer_;

	struct {
		struct Entry {
			DescriptorPool* pool {};
			DescriptorPoolSetEntry* entry {};
			u32 id {};
		};

		std::vector<Entry> entries;
		std::vector<IntrusivePtr<DescriptorPool>> pools;
		Entry selected {};
		DescriptorStateCopyPtr state {};
	} ds_;
};

} // namespace vil
