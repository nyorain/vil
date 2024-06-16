#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <memory>
#include <nytl/fwd/span.hpp>
#include <nytl/fwd/flags.hpp>

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
struct CompletedHook;
struct DescriptorCopyOp;
struct DescriptorCopyOp;
struct CopiedImage;
struct FrameSubmission;
struct QueueSubmitter;
struct BindSparseSubmission;

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
struct AccelStructState;
struct DescriptorSetCow;

struct MemoryBind;
struct FullMemoryBind;
struct SparseMemoryBind;
struct OpaqueSparseMemoryBind;
struct ImageSparseMemoryBind;

struct MatchVal;

struct ObjectTypeHandler;

enum class CommandCategory : u32;
using CommandCategoryFlags = nytl::Flags<CommandCategory>;

enum class SubmissionType : u8;

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
using AccelStructStatePtr = IntrusivePtr<AccelStructState>;

template<typename V, typename T>
decltype(auto) constexpr templatize(T&& value) {
	return std::forward<T>(value);
}

// serialize stuff
struct StateSaver;
struct StateLoader;
struct LoadBuf; // like readBuf but 'read' overloads throw on error

enum class MatchType {
	// mainly matches by identity of handles
	// there are some special cases, e.g. swapchain images
	identity,
	// like identity but also considers names, when handles are named,
	// apply deep matching. Useful for handle recreation, e.g. pipe reloading
	mixed,
	// do deep matching also for unnamed handles
	deep,
};

void onDeviceLost(Device& dev);

} // namespace

// spirv-cross
namespace spc {

class Compiler;
struct Resource;
struct SPIRConstant;
struct BuiltInResource;

} // namespace

namespace vil::vku {

struct LocalImageState;
struct LocalBufferState;
struct SyncScope;
struct BufferSpan;

} // namespace

// nytl/bytes
namespace nytl {

using ReadBuf = span<const std::byte>;
using WriteBuf = span<std::byte>;

} // namespace

// TODO: rename to something like VIL_VK_CHECK
#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assertm(result == VK_SUCCESS, "result: {}", result); \
	} while(0)

#define VK_CHECK_DEV(x, dev) do {\
		auto result = (x);\
		dlg_assertm(result == VK_SUCCESS, "result: {}", result); \
		if(result == VK_ERROR_DEVICE_LOST) { \
			onDeviceLost(dev); \
		} \
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
