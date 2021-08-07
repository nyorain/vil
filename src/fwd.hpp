#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nytl {

// span
// C++20: replace with std::span
constexpr const std::size_t dynamic_extent = std::size_t(-1);
template<typename T, std::size_t N = dynamic_extent> class span;
template<typename T, std::size_t N = dynamic_extent>
using Span = span<T, N>;

// flags
template<typename T, typename U = std::underlying_type_t<T>> class Flags;

} // namespace nytl

namespace vil {

using namespace nytl;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using std::size_t;

class Gui;
struct RenderBuffer;

struct Device;
struct Instance;
struct Handle;
struct DeviceHandle;
struct Swapchain;
struct Image;
struct ImageView;
struct Framebuffer;
struct RenderPass;
struct CommandBuffer;
struct Buffer;
struct BufferView;
struct QueryPool;
struct DeviceMemory;
struct Fence;
struct Event;
struct Semaphore;
struct CommandPool;
struct DescriptorPool;
struct DescriptorSet;
struct DescriptorSetState;
struct DescriptorSetLayout;
struct DescriptorUpdateTemplate;
struct ShaderModule;
struct Pipeline;
struct ComputePipeline;
struct GraphicsPipeline;
struct RayTracingPipeline;
struct PipelineLayout;
struct Sampler;
struct MemoryResource;
struct RenderPassDesc;
struct AccelStruct;
struct PipelineShaderStage;

struct Queue;
struct QueueFamily;
struct Submission;
struct SubmissionBatch;
struct CommandHook;
struct CommandHookSubmission;
struct CommandHookRecord;
struct CommandHookState;
struct CopiedImage;

struct ViewableImageCopy;

struct Command;
struct SectionCommand;
struct CommandRecord;
struct CommandBufferDesc;
struct CommandMemBlock;
struct CommandDescriptorSnapshot;
struct DrawCmdBase;
struct DispatchCommand;
struct BeginRenderPassCmd;
struct BuildAccelStructsCmd;
struct BuildAccelStructsIndirectCmd;

enum class CommandType : u32;
using CommandTypeFlags = nytl::Flags<CommandType>;

struct DisplayWindow;
struct Platform;
struct Overlay;
struct Draw;

struct SpirvData;
struct ThreadMemScope;

struct AccelTriangles;
struct AccelAABBs;
struct AccelInstances;

struct DescriptorStateRef {
	DescriptorSetState* state {};
	u32 binding {};
	u32 elem {};

	struct Hash {
		std::size_t operator()(const DescriptorStateRef& dsr) const noexcept;
	};

	friend inline bool operator==(const DescriptorStateRef& a, const DescriptorStateRef& b) {
		return a.state == b.state && a.binding == b.binding && a.elem == b.elem;
	}
};

struct DrawGuiImage;

} // namespace vil

#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assertm(result == VK_SUCCESS, "result: {}", result); \
	} while(0)
