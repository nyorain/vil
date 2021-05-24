#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <vk/vulkan.h>
#include <util/intrusive.hpp>

#include <memory>
#include <atomic>
#include <vector>
#include <optional>

namespace vil {

typedef struct SpvReflectShaderModule SpvReflectShaderModule;

struct XfbCapture {
	enum Type {
		typeFloat,
		typeInt,
		typeUint,
	};

	Type type;
	u32 columns {1};
	u32 vecsize {1};
	std::vector<u32> array {};
	u32 width;

	std::optional<u32> builtin {}; // spv11::Builtin, may be 0
	std::string name; // might be empty
	u32 offset; // total offset into xfb buffer
};

// We separate the description from the patched VkShaderModule since the description
// might be needed as long as there exists a pipe using this module.
// But we don't want to keep the patched VkShaderModule alive when the application's original
// module is destroyed to not waste memory.
struct XfbPatchDesc {
	std::vector<XfbCapture> captures;
	u32 stride {};
	std::atomic<u32> refCount {};
};

struct XfbPatchData {
	VkShaderModule mod {};
	std::string entryPoint {};
	IntrusivePtr<XfbPatchDesc> desc {};
};

XfbPatchData patchVertexShaderXfb(Device&, span<const u32> spirv,
	const char* entryPoint, std::string_view modName);

struct SpirvData {
	std::unique_ptr<SpvReflectShaderModule> reflection;
	std::atomic<u32> refCount {};

	~SpirvData();
};

struct ShaderModule : DeviceHandle {
	VkShaderModule handle {};

	// TODO: should keep this alive (longer than the ShaderModule lifetime)
	// so it's inspectable in the gui later on. But this would eat up *a lot*
	// of memory, games can have many thousand shader modules.
	// Maybe we could dump them to disk and read them when needed. Should also
	// just generate the Reflection on-demand then since that eats up quite
	// some data as well.
	std::vector<u32> spv;

	// Owend by us.
	// When the shader module is a vertex shader, we lazily create
	// transform-feedback version(s) of it. We might need multiple
	// versions in case the module has multiple entry points.
	std::vector<XfbPatchData> xfb;

	// Managed via shared ptr since it may outlive the shader module in
	// (possibly multiple) pipeline objects.
	IntrusivePtr<SpirvData> code;

	~ShaderModule();
};

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice                                    device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderModule*                             pShaderModule);

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
    VkDevice                                    device,
    VkShaderModule                              shaderModule,
    const VkAllocationCallbacks*                pAllocator);

} // namespace vil
