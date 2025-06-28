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

// TODO(low): the representation is counter-intuitive and makes our lives
// harder a couple of times in the implementation. 'vertexOffset' should
// always mean vertexOffset and 'indexOffset' (instead of offset) only be
// available for indexed drawing.
struct DrawParams {
	std::optional<VkIndexType> indexType {}; // nullopt for non-indexed draw
	u32 offset {}; // firstVertex or firstIndex
	u32 drawCount {}; // vertexCount or indexCount
	i32 vertexOffset {}; // only for indexed drawing

	// TODO: correctly implement multi-instance support
	u32 instanceID {};
};

class VertexViewer {
public:
	static constexpr auto colFrustum = 0xAAFFAACCu; // red-ish
	static constexpr auto colSelected = 0xFFAAAACCu; // green-ish
	static constexpr auto colWireframe = 0xFFFFFFFFu; // white
	static constexpr auto colShade = 0u;

public:
	~VertexViewer();

	void init(Gui& gui);

	// Returns whether hook need update
	bool displayInput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt,
		CommandViewer&);
	bool displayOutput(Draw&, const DrawCmdBase&, const CommandHookState&, float dt,
		CommandViewer&);
	void displayTriangles(Draw&, const OwnBuffer& buf, const AccelTriangles&, float dt);
	void displayInstances(Draw&, const AccelInstances&, float dt,
		std::function<AccelStructStatePtr(u64)> blasResolver);

	u32 selectedCommand() const { return selectedCommand_; }
	bool showAll() const { return showAll_; }

private:
	void centerCamOnBounds(const AABB3f& bounds);
	VkPipeline getOrCreatePipe(VkFormat format, u32 stride,
		VkPrimitiveTopology topo, VkPolygonMode polygonMode,
		bool enableDepth);
	VkPipeline createPipe(VkFormat format, u32 stride,
		VkPrimitiveTopology topo, VkPolygonMode polygonMode,
		bool enableDepth);
	void createFrustumPipe();

	void updateInput(float dt);
	void updateFPCam(float dt);
	void updateArcballCam(float dt);

	struct DrawData {
		VertexViewer* self {};

		VkPrimitiveTopology topology;
		std::vector<vku::BufferSpan> vertexBuffers;

		DrawParams params;
		vku::BufferSpan indexBuffer; // only for indexed drawing
		u32 selectedVertex {0xFFFFFFFFu};

		Vec2f offset {};
		Vec2f size {};

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
	bool showSettings(bool allowShowAll = false);
	void displayVertexID(u32 id);
	void displayDebugPopup(u32 vertexID, CommandViewer& viewer,
		const DrawCmdBase& cmd);

private:
	Gui* gui_ {};

	VkShaderModule vertShader_ {};
	VkShaderModule fragShader_ {};

	Camera cam_ {};
	bool rotating_ {};
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
		VkPolygonMode polygon {};
		bool enableDepth {};
	};

	struct PCData {
		Mat4f matrix;
		u32 useW;
		float scale;
		u32 flipY;
		u32 color;
	};

	std::vector<Pipe> pipes_ {};
	DrawData drawData_;

	u32 selectedVertex_ {};

	u32 selectedCommand_ {};
	std::vector<DrawData> drawDatas_;

	u32 precision_ {5u};
	bool doClear_ {false};
	bool flipY_ {};
	bool arcball_ {true}; // whether to use instead of first person cam
	bool wireframe_ {false};
	bool showAll_ {};
	float arcOffset_ {1.f};
};

} // namespace vil

