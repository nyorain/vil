#pragma once

#include <device.hpp>
#include <guidraw.hpp>
#include <variant>
#include <string>

namespace fuen {

struct ResourceGui {
	void select(Handle& handle);
	void draw(Draw&);
	void destroyed(const Handle&);
	~ResourceGui();

	Gui* gui_ {};
	std::string search_;
	int filter_ {0};

	Handle* handle_ {};
	struct {
		Image* object {};
		VkImageSubresourceRange newSubres {};
		VkImageSubresourceRange subres {};
		VkImageView view {};

		u32 flags {};

		DrawGuiImage draw {};
	} image_;

	struct {
		Buffer* handle {};
		std::vector<std::byte> lastRead;
		std::string layoutText;
		std::vector<std::pair<std::string, VkFormat>> layout;
	} buffer_;

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

	void drawHandleDesc(Draw&, Handle& handle);
};

} // namespace fuen
