#pragma once

#include <device.hpp>
#include <variant>
#include <string>

namespace fuen {

struct ResourceGui {
	void draw(Draw&);

	template<typename T>
	void select(T& handle) {
		handle_ = {&handle};
	}

	// for resource overview
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

	HandleVariant handle_;

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

};

} // namespace fuen
