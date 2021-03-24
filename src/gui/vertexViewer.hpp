#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <vk/vulkan.h>
#include <util/vec.hpp>
#include <util/camera.hpp>
#include <util/span.hpp>
#include <vector>
#include <optional>

namespace vil {

struct VertexViewer {

	~VertexViewer();

	void init(Device& dev, VkRenderPass rp);
	VkPipeline createPipe(VkFormat format, u32 stride, VkPrimitiveTopology topo);

	// Assumes to be inside a render pass with
	// - a depth attachment
	// - a single color attachment
	// - viewport and scissor dynamic state bound
	// Uses the current imgui context.
	// TODO: deprecate?
	void imGuiDraw(VkCommandBuffer cb, const GraphicsPipeline& src,
		const CommandHookState& copies, std::optional<VkIndexType>,
		u32 offset, u32 drawCount, u32 vertexOffset,
		Vec2f canvasOffset, Vec2f canvasSize);

	struct DrawData {
		VkPrimitiveTopology topology;
		VkPipelineVertexInputStateCreateInfo vertexInfo;
		span<const BufferSpan> vertexBuffers;

		u32 offset; // firstVertex or firstIndex
		u32 drawCount; // vertexCount or instanceCount

		std::optional<VkIndexType> indexType; // nullopt for non-indexed draw
		BufferSpan indexBuffer; // only for indexed drawing
		u32 vertexOffset; // only for indexed drawing

		Vec2f canvasOffset;
		Vec2f canvasSize;

		float scale {1.f};
		bool useW {false};
	};

	void displayInput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt);
	void displayOutput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt);

	void imGuiDraw(VkCommandBuffer cb, const DrawData& data);
	void updateInput(float dt);

private:
	Device* dev_ {};
	VkRenderPass rp_ {};

	VkShaderModule vertShader_ {};
	VkShaderModule fragShader_ {};

	Camera cam_ {};
	bool rotating_ {};
	Vec2f lastMousPos_ {};
	float yaw_ {};
	float pitch_ {};

	Mat4f viewProjMtx_ {};

	// OwnBuffer ubo_;
	// void* uboMap_ {};

	// VkDescriptorSetLayout dsLayout_ {};
	// VkDescriptorSet ds_ {};
	VkPipelineLayout pipeLayout_ {};

	// NOTE: could use way less pipes and instead just use a storage buffer
	// to assemble the vertices from in our vertex shader.
	// TODO: could at least cache shader modules.
	struct Pipe {
		VkFormat format {};
		u32 stride {};
		VkPrimitiveTopology topology {};
		VkPipeline pipe {};
	};

	std::vector<Pipe> pipes_ {};

	struct VertexDrawData {
		VertexViewer* self;
		const CommandHookState* state;
		VkCommandBuffer cb;
		Vec2f offset;
		Vec2f size;
		const DrawCmdBase* cmd;
		u32 vertexCount;
	} vertexDrawData_;
};

} // namespace vil

