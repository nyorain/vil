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

// buffmt.hpp
struct Type;

struct ShaderSpecialization {
	std::vector<VkSpecializationMapEntry> entries;
	std::vector<std::byte> data {};

	VkSpecializationInfo vkInfo() const;
};

ShaderSpecialization createShaderSpecialization(const VkSpecializationInfo*);
bool operator==(const ShaderSpecialization& a, const ShaderSpecialization& b);

struct XfbCapture {
	~XfbCapture(); // = default
	XfbCapture(XfbCapture&&) noexcept = default;
	XfbCapture& operator=(XfbCapture&&) noexcept = default;

	std::unique_ptr<Type> type;
	std::optional<u32> builtin {}; // spv11::Builtin, may be 0
	std::string name; // might be empty
	u32 offset; // total offset into xfb buffer
	std::vector<u32> arrayVals; // for type.array
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

XfbPatchRes patchSpirvXfb(spc::Compiler&, const char* entryPoint);
XfbPatchData patchShaderXfb(Device&, spc::Compiler& compiled,
	const char* entryPoint, std::string_view modName);

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

struct SpecializationConstantDefault {
	u32 constantID;
	std::unique_ptr<spc::SPIRConstant> constant;
};

struct ShaderModule : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_SHADER_MODULE;

	VkShaderModule handle {};

	// NOTE: in most cases, don't access directly, see 'accessReflection' below.
	// Need to use proper specialization constants.
	// Only to be used while the device mutex is locked, otherwise we can't
	// sync access.
	std::unique_ptr<spc::Compiler> compiled;
	std::vector<SpecializationConstantDefault> constantDefaults;
	u64 spirvHash {};

	// Owend by us.
	// When the shader module is a vertex shader, we lazily create
	// transform-feedback version(s) of it. We might need multiple
	// versions in case the module has multiple entry points.
	std::vector<XfbPatchData> xfb;

	ShaderModule(); // = default
	~ShaderModule();
	void clearXfb();
};

// Will set the given specialization, entryPoint and execution model into
// '*mod.compiled'.
// Might still need to call spc::Compiler::update_active_builtins() after this,
// if active builtins are accessed.
// Must only be called while the device mutex is locked for synchronization
// of mod.compiled.
spc::Compiler& specializeSpirv(ShaderModule& mod,
		const ShaderSpecialization& specialization, const std::string& entryPoint,
		u32 spvExecutionModel);
std::unique_ptr<spc::Compiler> copySpecializeSpirv(const ShaderModule& mod,
		const ShaderSpecialization& specialization, const std::string& entryPoint,
		u32 spvExecutionModel);

struct ShaderObject : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_SHADER_EXT;

	VkShaderEXT handle {};
	std::vector<IntrusivePtr<DescriptorSetLayout>> dsLayouts;
};

// API
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice                                    device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderModule*                             pShaderModule);

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
    VkDevice                                    device,
    VkShaderModule                              shaderModule,
    const VkAllocationCallbacks*                pAllocator);

// VK_EXT_shader_object
VKAPI_ATTR VkResult VKAPI_CALL CreateShadersEXT(
    VkDevice                                    device,
    uint32_t                                    createInfoCount,
    const VkShaderCreateInfoEXT*                pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderEXT*                                pShaders);

VKAPI_ATTR void VKAPI_CALL DestroyShaderEXT(
    VkDevice                                    device,
    VkShaderEXT                                 shader,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL GetShaderBinaryDataEXT(
    VkDevice                                    device,
    VkShaderEXT                                 shader,
    size_t*                                     pDataSize,
    void*                                       pData);

} // namespace vil
