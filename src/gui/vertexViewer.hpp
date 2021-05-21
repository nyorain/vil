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

struct AABB3f {
	Vec3f pos;
	Vec3f extent; // 0.5 * size
};

struct DrawParams {
	std::optional<VkIndexType> indexType {}; // nullopt for non-indexed draw
	u32 offset {}; // firstVertex or firstIndex
	u32 drawCount {}; // vertexCount or indexCount
	u32 vertexOffset {}; // only for indexed drawing
};

struct VertexViewer {
	~VertexViewer();

	void init(Device& dev, VkRenderPass rp);

	void displayInput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt);
	void displayOutput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt);

	void updateInput(float dt);

private:
	void centerCamOnBounds(const AABB3f& bounds);
	VkPipeline createPipe(VkFormat format, u32 stride, VkPrimitiveTopology topo);

	struct DrawData {
		VkPrimitiveTopology topology;
		VkPipelineVertexInputStateCreateInfo vertexInfo;
		std::vector<BufferSpan> vertexBuffers;

		DrawParams params;
		BufferSpan indexBuffer; // only for indexed drawing

		Vec2f canvasOffset;
		Vec2f canvasSize;

		float scale {1.f};
		bool useW {false};

		VkCommandBuffer cb {};
	};

	// Assumes to be inside a render pass with
	// - a depth attachment
	// - a single color attachment
	// - viewport and scissor dynamic state bound
	// Uses the current imgui context.
	void imGuiDraw(const DrawData& data);

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
	DrawData drawData_;
};

} // namespace vil

