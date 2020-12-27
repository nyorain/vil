#pragma once

#include <device.hpp>
#include <guidraw.hpp>
#include <variant>
#include <string>

namespace fuen {

struct ResourceGui {
	void draw(Draw&);
	void destroyed(const Handle&);
	~ResourceGui();

	template<typename T>
	void select(T& handle) {
		handle_ = {&handle};
	}

	Gui* gui_ {};
	std::string search_;
	int filter_ {0};

	// TODO: use just DeviceHandle*? and then do everything else via 'objectType'
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
		ShaderModule*,
		BufferView*,
		QueryPool*,
		Fence*,
		Semaphore*,
		Event*>;

	HandleVariant handle_;

	struct {
		Image* object {};
		VkImageSubresourceRange newSubres {};
		VkImageSubresourceRange subres {};
		VkImageView view {};

		u32 flags {};

		DrawGuiImage draw {};
	} image_;

	// enum class BufferLayoutType {
	// 	f1, f2, f3, f4,
	// 	d1, d2, d3, d4,
	// 	i1, i2, i3, i4,
	// 	u1, u2, u3, u4,
	// 	mat2, mat3, mat4,
	// 	eBool
	// };

	struct {
		Buffer* handle {};
		std::vector<std::byte> lastRead;
		std::string layoutText;
		std::vector<std::pair<std::string, VkFormat>> layout;
		// VkDeviceSize offset {};
		// VkDeviceSize size {};
		// std::vector<BufferLayoutType> layout;
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
};

} // namespace fuen
