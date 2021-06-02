#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <vk/vulkan.h>
#include <variant>
#include <vector>
#include <unordered_set>
#include <string>

namespace vil {

class ResourceGui {
public:
	~ResourceGui();

	void select(Handle& handle);
	void draw(Draw&);
	void destroyed(const Handle&);

	void recordPreRender(Draw&);
	void recoredPostRender(Draw&);

private:
	friend class Gui;

	Gui* gui_ {};
	std::string search_;
	VkObjectType filter_ {VK_OBJECT_TYPE_IMAGE};

	// The list of currently displayed handles
	std::vector<Handle*> handles_;
	std::unordered_set<const Handle*> destroyed_;

	bool firstUpdate_ {true};
	Handle* handle_ {};

	float listWidth_ {200.f};

	struct {
		Image* object {};
		VkImageView view {};
		DrawGuiImage draw {};

		unsigned level {};
		VkImageAspectFlagBits aspect {};

		OwnBuffer readPixelBuffer {};
		VkOffset3D readPixelOffset {};
	} image_;

	struct {
		Buffer* handle {};
		VkDeviceSize offset {};
		VkDeviceSize size {};

		std::vector<std::byte> lastRead;
		std::string layoutText;
		std::vector<std::pair<std::string, VkFormat>> layout;
	} buffer_;

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

	void drawHandleDesc(Draw&, Handle& handle);
};

} // namespace vil
