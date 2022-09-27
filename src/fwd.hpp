#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <memory>

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
struct DescriptorSetLayout;
struct DescriptorStateRef;
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
struct DescriptorPoolSetEntry;

struct Queue;
struct QueueFamily;
struct Submission;
struct SubmissionBatch;
struct CommandHook;
struct CommandHookSubmission;
struct CommandHookRecord;
struct CommandHookState;
struct LocalCapture;
struct CommandHookOps;
struct DescriptorCopyOp;
struct DescriptorCopyOp;
struct CopiedImage;
struct FrameSubmission;
struct QueueSubmitter;

struct ViewableImageCopy;

struct Command;
struct ParentCommand;
struct SectionCommand;
struct RootCommand;
struct UsedHandle;
struct CommandRecord;
struct CommandBufferDesc;
struct CommandMemBlock;
struct CommandDescriptorSnapshot;
struct DrawCmdBase;
struct DispatchCommand;
struct BeginRenderPassCmd;
struct BeginRenderingCmd;
struct BuildAccelStructsCmd;
struct BuildAccelStructsIndirectCmd;
struct DescriptorSetCow;

struct Matcher;

struct ObjectTypeHandler;

enum class CommandType : u32;
using CommandTypeFlags = nytl::Flags<CommandType>;

struct DisplayWindow;
struct Platform;
struct Overlay;
struct Draw;

struct ThreadMemScope;
struct LinAllocScope;
struct LinAllocator;

struct AccelTriangles;
struct AccelAABBs;
struct AccelInstances;

struct DrawGuiImage;

struct RecordBuilder;

struct CommandRecordMatch;
struct FrameSubmissionMatch;
struct FrameMatch;
struct FindResult;
struct CommandSectionMatch;

class CommandSelection;

enum class AttachmentType : u8;

enum class LocalCaptureBits : u32;
using LocalCaptureFlags = Flags<LocalCaptureBits>;

template<typename T> struct IntrusiveWrappedPtr;
template<typename T> struct WrappedHandle;

template<typename T, typename H> class HandledPtr;
template<typename T, typename Deleter = std::default_delete<T>> struct RefCountHandler;
template<typename T> struct FinishHandler;

template<typename T, typename D = std::default_delete<T>>
using IntrusivePtr = HandledPtr<T, RefCountHandler<T, D>>;

template<typename T> using FinishPtr = HandledPtr<T, FinishHandler<T>>;

using CommandBufferPtr = IntrusiveWrappedPtr<CommandBuffer>;

} // namespace vil


// spirv-cross
namespace spc {

class Compiler;
struct Resource;
struct SPIRConstant;
struct BuiltInResource;

} // namespace spc

// TODO: rename to something like VIL_VK_CHECK
#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assertm(result == VK_SUCCESS, "result: {}", result); \
	} while(0)


#ifdef VIL_DEBUG
	#define VIL_DEBUG_ONLY(x) x
#else // VIL_DEBUG(x)
	#define VIL_DEBUG_ONLY(x)
#endif // VIL_DEBUG(x)

#if __cplusplus >= 201902
	#define VIL_LIKELY [[likely]]
	#define VIL_UNLIKELY [[unlikely]]
#else
	#define VIL_LIKELY
	#define VIL_UNLIKELY
#endif
