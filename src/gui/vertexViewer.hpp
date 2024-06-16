#pragma once

#include <fwd.hpp>
#include <gui/render.hpp>
#include <vk/vulkan.h>
#include <util/camera.hpp>
#include <nytl/vec.hpp>
#include <nytl/span.hpp>
#include <nytl/matOps.hpp>
#include <vector>
#include <optional>

namespace vil {

struct AABB3f {
	Vec3f pos;
	Vec3f extent; // 0.5 * size
};

// TODO: the representation is counter-intuitive and makes our lives
// harder a couple of times in the implementation. 'vertexOffset' should
// always mean vertexOffset and 'indexOffset' (instead of offset) only be
// available for indexed drawing.
struct DrawParams {
	std::optional<VkIndexType> indexType {}; // nullopt for non-indexed draw
	u32 offset {}; // firstVertex or firstIndex
	u32 drawCount {}; // vertexCount or indexCount
	u32 vertexOffset {}; // only for indexed drawing

	// TODO: correctly implement multi-instance support
	u32 instanceID {};
};

struct VertexViewer {
	~VertexViewer();

	void init(Gui& gui);

	void displayInput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt);
	void displayOutput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt);
	void displayTriangles(Draw&, const OwnBuffer& buf, const AccelTriangles&, float dt);
	void displayInstances(Draw&, const AccelInstances&, float dt,
		std::function<AccelStructStatePtr(u64)> blasResolver);

	void updateInput(float dt);

private:
	void centerCamOnBounds(const AABB3f& bounds);
	VkPipeline createPipe(VkFormat format, u32 stride, VkPrimitiveTopology topo);
	void createFrustumPipe();

	struct DrawData {
		VertexViewer* self {};

		VkPrimitiveTopology topology;
		std::vector<BufferSpan> vertexBuffers;

		DrawParams params;
		BufferSpan indexBuffer; // only for indexed drawing

		Vec2f canvasOffset;
		Vec2f canvasSize;

		float scale {1.f};
		bool useW {false};
		bool drawFrustum {false};
		bool clear {true};

		Mat4f mat = nytl::identity<4, float>();

		struct {
			std::vector<VkVertexInputBindingDescription> bindings;
			std::vector<VkVertexInputAttributeDescription> attribs;
		} vertexInput;

		VkCommandBuffer cb {};
	};

	// Assumes to be inside a render pass with
	// - a depth attachment
	// - a single color attachment
	// - viewport and scissor dynamic state bound
	// Uses the current imgui context.
	void imGuiDraw(const DrawData& data);

private:
	Gui* gui_ {};

	VkShaderModule vertShader_ {};
	VkShaderModule fragShader_ {};

	Camera cam_ {};
	bool rotating_ {};
	Vec2f lastMousPos_ {};
	float yaw_ {};
	float pitch_ {};

	float speed_ {1.f};
	float near_ {-0.001f};
	float far_ {-10000.f};

	Mat4f viewProjMtx_ {};

	VkPipelineLayout pipeLayout_ {};
	VkPipeline frustumPipe_ {};

	// NOTE: could use way less pipes and instead just use a storage buffer
	// to assemble the vertices from in our vertex shader. Or alternatively
	// use extended dynamic state extension.
	struct Pipe {
		VkFormat format {};
		u32 stride {};
		VkPrimitiveTopology topology {};
		VkPipeline pipe {};
	};

	std::vector<Pipe> pipes_ {};
	DrawData drawData_;

	u32 selectedID_ {};
	std::vector<DrawData> drawDatas_;
};

} // namespace vil

