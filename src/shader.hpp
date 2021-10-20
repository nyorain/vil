#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <vk/vulkan.h>
#include <util/intrusive.hpp>
#include <util/debugMutex.hpp>

#include <memory>
#include <atomic>
#include <vector>
#include <optional>

namespace vil {

struct ShaderSpecialization {
	std::vector<VkSpecializationMapEntry> entries;
	std::vector<std::byte> data {};
};

ShaderSpecialization createShaderSpecialization(const VkSpecializationInfo*);
bool operator==(const ShaderSpecialization& a, const ShaderSpecialization& b);

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
	std::string entryPoint {};
	ShaderSpecialization spec {};

	VkShaderModule mod {};
	IntrusivePtr<XfbPatchDesc> desc {};
};

struct XfbPatchRes {
	std::vector<u32> spirv;
	IntrusivePtr<XfbPatchDesc> desc {};
};

struct SpirvData {
	std::atomic<u32> refCount {};

	// for access to 'compiled'. Locking other mutexes while holding
	// this one can lead to a deadlock.
	mutable DebugMutex mutex;
	// NOTE: don't access directly, see 'accessReflection' below.
	// Need to use proper specialization constants and therefore
	// make sure to lock access. We don't want to store the reflection
	// per used pipeline but just once so we have to set the corresponding
	// specialization constants every time we want to look something up.
	std::unique_ptr<spc::Compiler> compiled;

	struct SpecializationConstantDefault {
		u32 constantID;
		std::unique_ptr<spc::SPIRConstant> constant;
	};

	std::vector<SpecializationConstantDefault> constantDefaults;

	~SpirvData();
};

XfbPatchRes patchSpirvXfb(span<const u32> spirv, const char* entryPoint,
	spc::Compiler&);
XfbPatchData patchShaderXfb(Device&, span<const u32> spirv,
	const char* entryPoint, spc::Compiler& compiled,
	std::string_view modName);

// RAII wrapper around an access to a spc::Compiler object.
// All accesses are exclusive since we always need to set specialization
// constants before reading anything.
struct ShaderReflectionAccess {
	SpirvData* data;
	std::unique_lock<DebugMutex> lock;

	spc::Compiler& get() { return *data->compiled; }
};

// Returns a name for the given set, binding in the given module.
struct BindingNameRes {
	enum class Type {
		valid,
		notfound,
		unnamed,
	};

	Type type;
	std::string name;
};

BindingNameRes bindingName(const spc::Compiler&, u32 setID, u32 bindingID);

// Returns the resource associated with the given set, binding, if present.
std::optional<spc::Resource> resource(const spc::Compiler&,
	u32 setID, u32 bindingID, VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM);
std::optional<spc::Resource> resource(const spc::Compiler&, u32 varID);
std::optional<spc::BuiltInResource> builtinResource(const spc::Compiler&, u32 varID);

struct ShaderModule : DeviceHandle {
	VkShaderModule handle {};

	// TODO: should keep this alive (longer than the ShaderModule lifetime)
	// so it's inspectable in the gui later on. But this would eat up *a lot*
	// of memory, games can have many thousand shader modules.
	// Maybe we could dump them to disk and read them when needed. Should also
	// just generate the Reflection on-demand then since that eats up quite
	// some data as well.
	// TODO: we probably don't need this, just use code->compiler->get_ir().code
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

// Might still need to call spc::Compiler::update_active_builtins() after this,
// if builtins are accessed.
ShaderReflectionAccess accessReflection(SpirvData& mod,
		const ShaderSpecialization& specialization, const std::string& entryPoint,
		u32 spvExecutionModel);

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
